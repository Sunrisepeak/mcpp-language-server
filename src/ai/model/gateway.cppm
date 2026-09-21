// A ModelClient backed by the mcppls-model child process (tools/model-gateway/PROTOCOL.md, design 7.5
// M2): one JSON object per line on the child's stdin/stdout, requests correlated by id so calls can
// be pipelined, a per-request timeout that sends `cancel` and fails the call rather than blocking
// forever, and a clean report when the child exits. Never logs a message's content (design 11).
export module mcppls.ai.model.gateway;

import std;
import mcppls.base.error;
import mcppls.ai.model.source;

export namespace mcppls::ai::model {

struct GatewayOptions {
    std::string executable;                 // absolute path to mcppls-model (or a compatible gateway)
    std::vector<std::string> arguments;
    std::string workDirectory;               // empty: this process's current directory
    std::chrono::milliseconds requestTimeout { std::chrono::seconds { 60 } };
    std::chrono::milliseconds stopGrace { std::chrono::milliseconds { 500 } };
};

class GatewayClient : public ModelClient {
public:
    explicit GatewayClient(GatewayOptions options);
    ~GatewayClient() override;
    GatewayClient(const GatewayClient&) = delete;
    GatewayClient& operator=(const GatewayClient&) = delete;

public:
    // Spawns the child and completes the `initialize` handshake. Safe to call more than once;
    // `complete` calls it on demand, so most callers never need to.
    base::Result<void> start();
    // Closes the child's input, waits `stopGrace`, then terminates it if it has not exited.
    void stop();
    bool running() const;

    base::Result<CompletionResult> complete(const CompletionRequest& request) override;
    Usage usage() const override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace mcppls::ai::model
