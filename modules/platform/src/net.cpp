module mcppls.platform.net;

import std;
import mcpplibs.os;
import mcppls.base.error;

namespace mcppls::platform::net {

namespace osnet = mcpplibs::os::net;
namespace osipc = mcpplibs::os::ipc;

std::string random_token(std::size_t bytes) { return mcpplibs::os::random::hex(bytes).value_or(std::string {}); }

// The connection is read by one thread while others write, shut it down or close it (a daemon
// writing a reply while its reader waits for the next request). mcpplibs.os's connection carries
// both directions at once on every system (openkal-windows' sockets are overlapped since 0.13), and
// its `shutdown` wakes a reader from another thread; the handle is released with the state, once
// nothing can be using it.
struct Connection::State {
    osipc::connection connection;
    std::atomic<bool> open { true };
};

Connection::Connection() = default;
Connection::Connection(Connection&& other) noexcept = default;
Connection& Connection::operator=(Connection&& other) noexcept = default;
Connection::~Connection() { close(); }

base::Result<Connection> Connection::connect_local(int port) {
    auto connected = osnet::connect_loopback(static_cast<std::uint16_t>(port));
    if (!connected) return base::fail("net-connect", std::format("cannot connect to 127.0.0.1:{}: {}", port, connected.error().message()));
    Connection made;
    made.state_ = std::make_unique<State>();
    made.state_->connection = std::move(*connected);
    return made;
}

bool Connection::valid() const { return state_ && state_->open; }

base::Result<std::string> Connection::read() {
    if (!valid()) return std::string {};
    auto got = state_->connection.read(16384);
    if (!got) {
        if (got.error().code == mcpplibs::os::errc::closed || !state_->open) return std::string {};
        return base::fail("net-read", std::format("read failed: {}", got.error().message()));
    }
    return std::move(*got);
}

base::Result<void> Connection::write(std::string_view bytes) {
    if (!valid()) return base::fail("net-write", "the connection is closed");
    if (auto written = state_->connection.write(bytes); !written)
        return base::fail("net-write", std::format("write failed: {}", written.error().message()));
    return {};
}

void Connection::shutdown_write() {
    if (valid()) state_->connection.shutdown_write();
}

void Connection::close() {
    // Both directions end, which wakes a thread reading; the handle is released with the state.
    if (!state_ || !state_->open.exchange(false)) return;
    state_->connection.shutdown();
}

struct Listener::State {
    osnet::loopback_listener listener;
};

Listener::Listener() = default;
Listener::Listener(Listener&& other) noexcept = default;
Listener& Listener::operator=(Listener&& other) noexcept = default;
Listener::~Listener() { close(); }

base::Result<Listener> Listener::listen_local() {
    auto listening = osnet::loopback_listener::listen(0);
    if (!listening) return base::fail("net-listen", std::format("cannot listen on 127.0.0.1: {}", listening.error().message()));
    Listener made;
    made.state_ = std::make_unique<State>();
    made.state_->listener = std::move(*listening);
    return made;
}

int Listener::port() const { return state_ ? static_cast<int>(state_->listener.port()) : 0; }

base::Result<Connection> Listener::accept() {
    if (!state_) return base::fail("net-accept", "the listener is closed");
    auto accepted = state_->listener.accept();
    if (!accepted) return base::fail("net-accept", std::format("accept failed: {}", accepted.error().message()));
    Connection made;
    made.state_ = std::make_unique<Connection::State>();
    made.state_->connection = std::move(*accepted);
    return made;
}

void Listener::close() {
    if (state_) state_->listener.close();
}

} // namespace mcppls::platform::net
