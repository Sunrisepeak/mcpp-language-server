module mcppls.platform.net;

import std;
import openkal.types;
import openkal.stream;
import openkal.net;
import openkal.random;
import openkal.timeout;
import mcppls.os;
import mcppls.base.error;

namespace mcppls::platform::net {

namespace {

// ⚠️ ON WINDOWS ONE OPERATION AT A TIME REACHES A CONNECTION'S HANDLE. openkal makes a socket without
// the overlapped attribute so that ReadFile and WriteFile wait as they do on a pipe, and the system
// serializes the operations on such a handle: a read waiting for bytes holds back a write from another
// thread until bytes come. A daemon writing a reply while its reader waits for the next request, or a
// relay forwarding a request while it waits for the reply, would wait for each other for good. So there
// a read waits for readiness a slice at a time and reads only bytes that are there, and between slices
// it gives way to a write, a shutdown or a close waiting for the handle.
constexpr bool SERIALIZED_HANDLES { mcppls::os::FAMILY == mcppls::os::Family::windows };
constexpr kal_u64 SLICE_NANOSECONDS { 10'000'000 };
constexpr std::size_t WRITE_PIECE { 64 * 1024 };

kal_endpoint loopback(int port) {
    kal_endpoint endpoint {};
    endpoint.addr[0] = 127;
    endpoint.addr[3] = 1;
    endpoint.addr_len = 4;
    endpoint.port = static_cast<kal_u32>(port);
    return endpoint;
}

std::string describe(int code) {
    switch (code) {
    case kal_err_invalid: return "invalid argument";
    case kal_err_again: return "would block";
    case kal_err_io: return "input/output error";
    case kal_err_permission: return "permission denied";
    case kal_err_not_supported: return "not supported";
    case kal_err_closed: return "closed";
    case kal_err_not_found: return "refused";
    default: return std::format("openkal error {}", code);
    }
}

} // namespace

std::string random_token(std::size_t bytes) {
    std::vector<unsigned char> buffer(bytes);
    (void)kal_random_fill(buffer.data(), buffer.size());
    std::string token;
    for (unsigned char byte : buffer) token += std::format("{:02x}", byte);
    return token;
}

struct Connection::State {
    kal_net_conn connection {};
    std::mutex writeMutex;
    std::atomic<bool> open { true };
    std::atomic<int> waiting { 0 };   // operations other than the read that want the handle (SERIALIZED_HANDLES)
    ~State() { kal_net_close(connection); }

    // The reader's turn comes when nothing else wants the handle.
    void give_way() const {
        while (waiting.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    }
};

namespace {

// An operation other than the read, announced for as long as it runs so the reader keeps out of its way.
struct Turn {
    std::atomic<int>& waiting;
    explicit Turn(std::atomic<int>& counter) : waiting { counter } { ++waiting; }
    ~Turn() { --waiting; }
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
};

} // namespace

Connection::Connection() = default;
Connection::Connection(Connection&& other) noexcept = default;
Connection& Connection::operator=(Connection&& other) noexcept = default;
Connection::~Connection() { close(); }

base::Result<Connection> Connection::connect_local(int port) {
    const kal_endpoint to { loopback(port) };
    kal_net_conn connection {};
    const int result { kal_net_connect(&to, &connection) };
    if (result != kal_ok) return base::fail("net-connect", std::format("cannot connect to 127.0.0.1:{}: {}", port, describe(result)));
    Connection made;
    made.state_ = std::make_unique<State>();
    made.state_->connection = connection;
    return made;
}

bool Connection::valid() const { return state_ && state_->open; }

base::Result<std::string> Connection::read() {
    if (!valid()) return std::string {};
    std::array<char, 16384> buffer {};
    const kal_stream stream { kal_net_stream(state_->connection) };
    kal_intptr got { 0 };
    if constexpr (SERIALIZED_HANDLES) {
        while (true) {
            state_->give_way();
            if (!state_->open) return std::string {};
            got = kal_timeout_read(stream, buffer.data(), buffer.size(), SLICE_NANOSECONDS);
            if (got != -kal_err_again) break;
        }
        // A handle the bound cannot be applied to is read as it is.
        if (got == -kal_err_not_supported) got = kal_stream_read(stream, buffer.data(), buffer.size());
    } else {
        got = kal_stream_read(stream, buffer.data(), buffer.size());
    }
    if (got < 0) {
        if (static_cast<int>(-got) == kal_err_closed) return std::string {};
        return base::fail("net-read", std::format("read failed: {}", describe(static_cast<int>(-got))));
    }
    return std::string { buffer.data(), static_cast<std::size_t>(got) };
}

base::Result<void> Connection::write(std::string_view bytes) {
    if (!valid()) return base::fail("net-write", "the connection is closed");
    std::lock_guard lock { state_->writeMutex };
    const kal_stream stream { kal_net_stream(state_->connection) };
    std::size_t done { 0 };
    while (done < bytes.size()) {
        kal_intptr written { 0 };
        if constexpr (SERIALIZED_HANDLES) {
            {
                // Written once there is room and a piece at a time, so a write does not hold the handle
                // waiting for the peer to read.
                const Turn turn { state_->waiting };
                const std::size_t piece { std::min<std::size_t>(bytes.size() - done, WRITE_PIECE) };
                written = kal_timeout_write(stream, bytes.data() + done, piece, SLICE_NANOSECONDS);
                if (written == -kal_err_not_supported) written = kal_stream_write(stream, bytes.data() + done, piece);
            }
            if (written == -kal_err_again) {
                if (!state_->open) return base::fail("net-write", "the connection is closed");
                // No room yet: the reader's turn meanwhile. Two peers each waiting to write would otherwise
                // both keep their readers away, and neither side would ever make room for the other.
                std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
                continue;
            }
        } else {
            written = kal_stream_write(stream, bytes.data() + done, bytes.size() - done);
        }
        if (written <= 0) return base::fail("net-write", std::format("write failed: {}", describe(static_cast<int>(-written))));
        done += static_cast<std::size_t>(written);
    }
    return {};
}

void Connection::shutdown_write() {
    if (!valid()) return;
    const Turn turn { state_->waiting };
    (void)kal_net_shutdown(state_->connection, 2 /* KAL_SHUT_WRITE */);
}

void Connection::close() {
    // Both directions end, which wakes a thread reading; the handle itself is released with the state,
    // when nothing can be using it any more.
    if (!state_ || !state_->open.exchange(false)) return;
    const Turn turn { state_->waiting };
    (void)kal_net_shutdown(state_->connection, 3 /* KAL_SHUT_BOTH */);
}

struct Listener::State {
    kal_net_listener listener {};
    int port { 0 };
    bool open { true };
};

Listener::Listener() = default;
Listener::Listener(Listener&& other) noexcept = default;
Listener& Listener::operator=(Listener&& other) noexcept = default;
Listener::~Listener() { close(); }

base::Result<Listener> Listener::listen_local() {
    const kal_endpoint local { loopback(0) };
    kal_net_listener listener {};
    const int result { kal_net_listen(&local, &listener) };
    if (result != kal_ok) return base::fail("net-listen", std::format("cannot listen on 127.0.0.1: {}", describe(result)));
    kal_endpoint bound {};
    if (const int named = kal_net_listener_local(listener, &bound); named != kal_ok) {
        kal_net_close_listener(listener);
        return base::fail("net-listen", std::format("cannot read the port listened on: {}", describe(named)));
    }
    Listener made;
    made.state_ = std::make_unique<State>();
    made.state_->listener = listener;
    made.state_->port = static_cast<int>(bound.port);
    return made;
}

int Listener::port() const { return state_ ? state_->port : 0; }

base::Result<Connection> Listener::accept() {
    if (!state_ || !state_->open) return base::fail("net-accept", "the listener is closed");
    kal_net_conn connection {};
    const int result { kal_net_accept(state_->listener, &connection) };
    if (result != kal_ok) return base::fail("net-accept", std::format("accept failed: {}", describe(result)));
    Connection made;
    made.state_ = std::make_unique<Connection::State>();
    made.state_->connection = connection;
    return made;
}

void Listener::close() {
    if (!state_ || !state_->open) return;
    state_->open = false;
    kal_net_close_listener(state_->listener);
}

} // namespace mcppls::platform::net
