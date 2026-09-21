module mcppls.lsp.connection;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.platform.process;
import mcppls.lsp.jsonrpc;

namespace mcppls::lsp {

Connection::~Connection() { stop(std::chrono::milliseconds { 500 }); }

base::Result<std::unique_ptr<Connection>> Connection::start(platform::SpawnOptions options, MessageHandler onMessage,
                                                            ClosedHandler onClosed, ErrorLineHandler onErrorLine, Framing framing) {
    options.pipeInput = true;
    options.pipeOutput = true;
    options.pipeError = static_cast<bool>(onErrorLine);
    auto process = platform::Process::spawn(options);
    if (!process) return std::unexpected { process.error() };

    auto connection = std::make_unique<Connection>();
    connection->process_ = std::move(*process);
    connection->framing_ = framing;
    Connection* self { connection.get() };
    connection->reader_ = std::jthread { [self, framing, onMessage = std::move(onMessage), onClosed = std::move(onClosed)] {
        FrameReader reader;
        std::string pending;
        while (true) {
            auto chunk = self->process_.read_output();
            if (!chunk || chunk->empty()) break;
            if (framing == Framing::lines) {
                pending += *chunk;
                for (std::size_t newline { pending.find('\n') }; newline != std::string::npos; newline = pending.find('\n')) {
                    std::string_view line { std::string_view { pending }.substr(0, newline) };
                    if (line.ends_with('\r')) line.remove_suffix(1);
                    if (!line.empty()) {
                        Json message = Json::parse(line, nullptr, false);
                        if (message.is_discarded()) base::log::warning("dropped a line from a child that is not JSON");
                        else onMessage(std::move(message));
                    }
                    pending.erase(0, newline + 1);
                }
                continue;
            }
            reader.feed(*chunk);
            while (auto message = reader.next()) {
                if (!*message) {
                    base::log::warning("dropped a malformed frame from a child: {}", message->error().message);
                    continue;
                }
                onMessage(std::move(**message));
            }
        }
        self->closed_.store(true);
        if (onClosed) onClosed();
    } };
    if (onErrorLine) {
        connection->errorReader_ = std::jthread { [self, onErrorLine = std::move(onErrorLine)] {
            std::string pending;
            while (true) {
                auto chunk = self->process_.read_error();
                if (!chunk || chunk->empty()) break;
                pending += *chunk;
                std::size_t newline { 0 };
                while ((newline = pending.find('\n')) != std::string::npos) {
                    std::string_view line { std::string_view { pending }.substr(0, newline) };
                    if (line.ends_with('\r')) line.remove_suffix(1);
                    onErrorLine(line);
                    pending.erase(0, newline + 1);
                }
            }
            if (!pending.empty()) onErrorLine(pending);
        } };
    }
    return connection;
}

base::Result<void> Connection::send(const Json& message) {
    if (closed_.load()) return base::fail("connection-closed", "the peer has exited");
    return process_.write(framing_ == Framing::lines ? dump(message) + "\n" : encode_frame(message));
}

void Connection::stop(std::chrono::milliseconds grace) {
    if (!process_.valid()) return;
    process_.close_input();
    auto waited = process_.wait_for(grace);
    if (!waited || !waited->has_value()) {
        process_.terminate();
        // A peer can ignore being asked; one stuck in a module build did, and blocked this wait for good.
        auto asked = process_.wait_for(std::chrono::seconds { 2 });
        if (!asked || !asked->has_value()) process_.kill();
        (void)process_.wait();
    }
    if (reader_.joinable()) reader_.join();
    if (errorReader_.joinable()) errorReader_.join();
}

std::optional<int> Connection::exit_code() {
    auto waited = process_.wait_for(std::chrono::milliseconds { 0 });
    if (!waited || !waited->has_value()) return std::nullopt;
    return **waited;
}

} // namespace mcppls::lsp
