// A JSON-RPC peer running as a child process: messages written to its standard
// input, messages read from its standard output on a reader thread, and its
// standard error drained on another. Messages are LSP frames, or single lines
// the way the Model Context Protocol's standard streams carry them.
export module mcppls.lsp.connection;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.platform.process;
import mcppls.lsp.jsonrpc;

export namespace mcppls::lsp {

enum class Framing { content_length, lines };

class Connection {
public:
    using MessageHandler = std::function<void(Json message)>;
    using ClosedHandler = std::function<void()>;
    using ErrorLineHandler = std::function<void(std::string_view line)>;

private:
    platform::Process process_;
    std::jthread reader_;
    std::jthread errorReader_;
    std::atomic<bool> closed_ { false };
    Framing framing_ { Framing::content_length };

public:
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

public:
    // Handlers run on the connection's own threads. `onClosed` runs once, after
    // the peer's output ended.
    static base::Result<std::unique_ptr<Connection>> start(platform::SpawnOptions options, MessageHandler onMessage,
                                                           ClosedHandler onClosed, ErrorLineHandler onErrorLine = {},
                                                           Framing framing = Framing::content_length);
    base::Result<void> send(const Json& message);
    // Ends the peer: closes its input, waits up to `grace`, then terminates it.
    void stop(std::chrono::milliseconds grace);
    bool closed() const { return closed_.load(); }
    std::optional<int> exit_code();
};

} // namespace mcppls::lsp
