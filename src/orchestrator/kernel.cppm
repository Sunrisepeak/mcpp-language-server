// A headless session (overall design 4.4, 7.1): one workspace root served without an editor, for the
// entries that ask on behalf of agents and scripts (MCP, the command line). The kernel is the
// workspace's client: it opens documents from disk, sends LSP requests through the workspace's
// routing, answers what the engines ask the client, and keeps what they publish.
export module mcppls.orchestrator.kernel;

import std;
import nlohmann.json;
import mcppls.orchestrator.workspace;

export namespace mcppls::orchestrator {

using Json = nlohmann::json;

struct KernelOptions {
    SessionOptions session;   // payload, engine and trust options, and the composition root's engine factories
    std::string root;         // the workspace root directory
};

class Kernel {
public:
    static std::unique_ptr<Kernel> start(const KernelOptions& options);
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    Workspace& workspace();
    const std::string& root() const;   // canonical

    // An LSP request through the workspace's routing: the whole response message (with "result" or
    // "error"), or nullopt when none arrived within `timeout`.
    std::optional<Json> request(std::string_view method, Json params, std::chrono::milliseconds timeout);

    // Documents, by path (absolute, or relative to the root). open reads the file unless it is open;
    // at most MAX_OPEN_DOCUMENTS stay open, the least recently used closed first.
    static constexpr std::size_t MAX_OPEN_DOCUMENTS { 64 };
    std::string uri_of(std::string_view path) const;
    std::string absolute_path(std::string_view path) const;
    void open(std::string_view path);
    // An overlay: the engines see `text` instead of the file until it is closed or refreshed from disk.
    void change(std::string_view path, std::string text);
    void close(std::string_view path);
    // An overlay's document gets its file's content back.
    void revert(std::string_view path);
    // An open document is given its content again as a new version, so the engines build it again:
    // what it imports may have changed while it did not.
    void touch(std::string_view path);
    bool is_open(std::string_view path) const;
    std::optional<std::int64_t> version(std::string_view path) const;   // of an open document
    // Open documents whose file changed on disk (an agent writes files) are given the new content,
    // unless they carry an overlay. Returns the files that changed.
    std::vector<std::string> refresh();
    // The text the engines see: the open buffer, else the file.
    std::optional<std::string> text(std::string_view path) const;
    // Open documents whose content differs from their file.
    std::vector<std::string> overlays() const;

    // What the workspace published.
    Json status() const;                                            // the latest cxxModules/status, or null
    std::optional<Json> diagnostics(std::string_view uri) const;    // the latest published list
    int diagnostics_publishes(std::string_view uri) const;          // how many lists were published
    bool indexing() const;                                          // an engine reports work in progress
    int progress_begun() const;                                     // work reports begun so far

    // Processes events until `done` holds or `timeout` passes; true when it held.
    bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds timeout);
    // Until the status settles (ready, degraded or error) and no model is loading.
    bool wait_settled(std::chrono::milliseconds timeout);

    // Another entry's messages, posted from any thread, handed out on the loop (see EventKind::external).
    void post_external(Json message);
    void close_external();   // no more messages will be posted
    std::optional<Json> next_external(std::chrono::milliseconds timeout);
    // The first message `wanted` accepts, leaving the others queued in order: the response to a
    // request this process sent the other entry's client while one of its requests is being answered.
    std::optional<Json> take_external(const std::function<bool(const Json&)>& wanted, std::chrono::milliseconds timeout);
    bool input_closed() const;

    void shut_down();

private:
    Kernel() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcppls::orchestrator
