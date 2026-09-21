// Where the workspace's messages to a client go (overall design 6.3): a sink per client connection,
// and the ids that let a client's response to an engine's own request find that engine again.
export module mcppls.orchestrator.client;

import std;
import nlohmann.json;

export namespace mcppls::orchestrator {

using Json = nlohmann::json;

class ClientSink {
public:
    virtual ~ClientSink() = default;
    virtual void send(const Json& message) = 0;

    void reply(const Json& id, Json result);
    void reply_error(const Json& id, int code, std::string_view message);
    void notify(std::string_view method, Json params);
};

// One LSP frame per message on this process's standard output.
class StdioSink : public ClientSink {
public:
    void send(const Json& message) override;
};

// The id a request an engine of a root sends to the client travels under: the root's key, the
// engine's id and generation, and the engine's own id, so the session can find the engine again
// and rule out a stale engine when the client eventually responds.
std::string make_engine_request_key(std::string_view rootKey, std::string_view engineId, int generation, const Json& engineRequestId);

struct EngineRequestKey {
    std::string rootKey;
    std::string engineId;
    int generation { 0 };
    Json engineRequestId;
};
std::optional<EngineRequestKey> parse_engine_request_key(std::string_view key);

} // namespace mcppls::orchestrator
