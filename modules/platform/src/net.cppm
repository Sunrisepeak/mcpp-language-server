// Connections on this machine's loopback interface over openkal.net: what the workspace daemon
// listens on and what entries connect with (overall design 6.3, experiment X2). Nothing here reaches
// another machine: every endpoint is 127.0.0.1.
export module mcppls.platform.net;

import std;
import mcppls.base.error;

export namespace mcppls::platform::net {

class Connection {
public:
    Connection();
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    static base::Result<Connection> connect_local(int port);

    bool valid() const;
    // Blocks until bytes arrive; an empty string means the peer closed its side, or close() was called.
    // One thread reads while others write, shut down or close: a connection is used both ways at once.
    base::Result<std::string> read();
    base::Result<void> write(std::string_view bytes);   // thread-safe
    // No more writes: the peer reads the end of the stream, and can still answer.
    void shutdown_write();
    void close();

private:
    friend class Listener;
    struct State;
    std::unique_ptr<State> state_;
};

// Hex digits of `bytes` random bytes from the system's source: the daemon's connection token.
std::string random_token(std::size_t bytes = 16);

class Listener {
public:
    Listener();
    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    ~Listener();

    // On 127.0.0.1, at a port the system chooses.
    static base::Result<Listener> listen_local();

    int port() const;
    base::Result<Connection> accept();   // blocks
    void close();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace mcppls::platform::net
