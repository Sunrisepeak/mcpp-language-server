// mcppls-conformance: drives a language server through a fixture's scenario
// and reports each check (conformance/README.md).
//
//   mcppls-conformance run --server <mcppls> --fixture <dir> [--payload DIR] [--clangd PATH] [--kit DIR]
//                            [--msvc-env FILE] [--timeout SECONDS] [--keep] [--verbose]
//                            [--workspace-dir DIR] [--cache-dir DIR] [--measure FILE] [--expect-warm]
//                            [--navigation-budget SECONDS]
//   mcppls-conformance prepare <kind> [argument]      a fixture's own prepare step (scenario.json)
//   mcppls-conformance version
import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.glob;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.base.version;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.platform.env;
import mcppls.platform.process;
import mcppls.platform.task;
import mcppls.lsp.jsonrpc;
import mcppls.lsp.connection;

namespace base = mcppls::base;
namespace fs = mcppls::platform::fs;
namespace lsp = mcppls::lsp;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

// Lines appear as they happen, also when output is a pipe.
template <class... Args>
void say(std::format_string<Args...> format, Args&&... args) {
    std::cout << std::format(format, std::forward<Args>(args)...) << '\n' << std::flush;
}

struct Options {
    std::string server;
    std::string fixture;
    std::string payload;
    std::string clangd;
    std::string kit;
    std::string msvcEnvironment;   // "NAME=value" lines of a developer environment, for fixtures that build with MSVC
    std::chrono::seconds timeout { 180 };
    bool keep { false };
    bool verbose { false };
    std::string workspaceDirectory;   // reused across runs: the fixture is copied and prepared there once
    bool expectWarm { false };        // module-cache-reused checks require module files from an earlier run
    std::optional<double> navigationBudget;   // seconds from initialize to the first navigation that answers; more fails the run
    std::string cacheDirectory;       // the server's cache; empty: a fresh one beside the workspace
    std::string measureFile;          // where the checks' timings are written as JSON
    bool noDynamicWatch { false };    // usable plan W9.3: do not advertise didChangeWatchedFiles.dynamicRegistration
    // A client that is not this repository's own VS Code extension: no `experimental.cxxModules`,
    // only standard `window.workDoneProgress`. Zed, nvim, Helix and every other editor look like
    // this, and nothing tested it — which is how `$/progress` came to be sent only AFTER the gate
    // that asks whether the client understands `cxxModules/status`, i.e. only to the one client
    // that already had progress (cold-start plan 4.1).
    bool plainClient { false };
    // `--client`: the capabilities a real editor actually sends. `none` (no flag at all) keeps this
    // runner's own long-standing default, the full experimental.cxxModules block with no
    // initializationOptions, so every fixture that predates this option keeps behaving exactly as
    // it did. `--plain-client` is kept as the alias `plain`.
    enum class ClientProfile { none, vscode, neovim, zed, plain };
    ClientProfile client { ClientProfile::none };
    // `mcppls-devtools stress --seed N`: overrides every stress check's own "seed", so a matrix
    // run stays reproducible without editing every fixture's scenario.json.
    std::optional<std::uint64_t> stressSeed;
    // real-project plan RP2.1: a fixture with `"isolate-home": true` needs producer negotiation to
    // see only its own candidates, never whatever mcpp or xlings a machine happens to have installed
    // under the real $HOME/%USERPROFILE%. Empty until `run()` reads the scenario; once set, every
    // process the runner starts for the server under test uses it as HOME (and USERPROFILE).
    std::string isolatedHome;
};

// Replaces HOME (POSIX) and USERPROFILE (Windows) in a spawn's environment, so
// `mcppls::platform::dirs::home_directory()` -- and so producer negotiation's search of
// `<home>/.xlings/data/xpkgs` and `<home>/.mcpp/registry/data/xpkgs` (real-project plan RP2.1) --
// sees only what the fixture itself put there. A no-op when `options.isolatedHome` is empty, which
// is every fixture that predates it.
void apply_isolated_home(std::vector<std::string>& environment, const Options& options) {
    if (options.isolatedHome.empty()) return;
    const auto isHomeVariable = [](const std::string& entry) {
        return entry.starts_with("HOME=") || entry.starts_with("USERPROFILE=") || entry.starts_with("HOMEDRIVE=") || entry.starts_with("HOMEPATH=");
    };
    std::erase_if(environment, isHomeVariable);
    environment.push_back("HOME=" + options.isolatedHome);
    environment.push_back("USERPROFILE=" + options.isolatedHome);
}

// Whether this profile looks like a client with no `experimental.cxxModules` at all: no
// `cxxModules/status` arrives, so status checks make no sense and standard `$/progress` is what
// the run must prove instead (cold-start plan 4.1). True for `plain` and `zed` (Zed advertises no
// experimental capability of ours), for `--plain-client`, and false for `none` (this runner's own
// long-standing default) so every fixture written before `--client` existed is unaffected.
bool is_plain_like(const Options& options) {
    return options.plainClient || options.client == Options::ClientProfile::zed || options.client == Options::ClientProfile::plain;
}

std::string absolute(std::string_view path) {
    if (path.empty() || base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(fs::current_directory(), path);
}

void copy_tree(const std::string& from, const std::string& to) {
    (void)fs::create_directories(to);
    for (const auto& entry : fs::list_directory(from)) {
        const std::string target { base::join_path(to, base::file_name(entry)) };
        if (fs::is_directory(entry)) {
            copy_tree(entry, target);
        } else if (auto content = fs::read_file(entry)) {
            (void)fs::write_file(target, *content);
        }
    }
}

// Placeholders in prepare commands and server arguments.
struct Expansion {
    std::string workspace;
    std::string runnerDirectory;
    std::string payload;   // usable plan W9.4: the --payload this runner itself was given, if any
    std::string runner;    // this program, for fixtures prepared by `mcppls-conformance prepare`
    // real-project plan RP2.1: the isolated HOME a `"isolate-home": true` fixture's own prepare
    // step populates (e.g. with a candidate mcpp under `xim-x-mcpp/<version>/bin/`), empty otherwise.
    std::string home;
};

// "{exe}" is the executable suffix; "{env:NAME|fallback}" is a variable or the fallback;
// "{workspace}" is the fixture's scratch copy; "{runner-dir}" is where this program lives;
// "{payload}" is the runner's own --payload (usable plan W9.4's payload-corrupt fixture copies
// and mutates it, then points server-arguments' own --payload at the mutated copy);
// "{conformance}" is this program, whose `prepare` command generates what a fixture needs.
std::string expand(std::string word, const Expansion& expansion = {}) {
    word = base::replace_all(word, "{exe}", mcppls::os::EXECUTABLE_SUFFIX);
    word = base::replace_all(word, "{workspace}", expansion.workspace);
    word = base::replace_all(word, "{runner-dir}", expansion.runnerDirectory);
    word = base::replace_all(word, "{payload}", expansion.payload);
    word = base::replace_all(word, "{conformance}", expansion.runner);
    word = base::replace_all(word, "{home}", expansion.home);
    for (std::size_t at { word.find("{env:") }; at != std::string::npos; at = word.find("{env:", at)) {
        const std::size_t close { word.find('}', at) };
        if (close == std::string::npos) break;
        const std::string body { word.substr(at + 5, close - at - 5) };
        const std::size_t bar { body.find('|') };
        const std::string name { body.substr(0, bar) };
        std::string value { mcppls::platform::env::get(name).value_or("") };
        if (value.empty() && bar != std::string::npos) value = body.substr(bar + 1);
        word.replace(at, close - at + 1, value);
        at += value.size();
    }
    return word;
}

// The environment a prepare step runs in: this process's, with a developer environment laid over it when given.
std::optional<std::vector<std::string>> prepare_environment(const std::string& overlayFile) {
    if (overlayFile.empty()) return std::nullopt;
    auto text = fs::read_file(overlayFile);
    if (!text) return std::nullopt;
    const bool caseInsensitive { mcppls::os::FAMILY == mcppls::os::Family::windows };
    auto key = [&](std::string_view entry) {
        std::string name { entry.substr(0, entry.find('=')) };
        return caseInsensitive ? base::to_lower_ascii(name) : name;
    };
    std::vector<std::string> environment { mcppls::platform::env::variables() };
    for (auto line : base::split_lines(*text)) {
        line = base::trim(line);
        if (line.empty() || line.find('=') == std::string_view::npos || line.front() == '=') continue;
        std::erase_if(environment, [&](const std::string& entry) { return key(entry) == key(line); });
        environment.emplace_back(line);
    }
    return environment;
}

// Runs a prepare step in the workspace.
bool run_prepare(const Json& command, const std::string& workspace, bool verbose, const Expansion& expansion,
                 const std::optional<std::vector<std::string>>& environment) {
    if (!command.is_array() || command.empty()) return true;
    std::vector<std::string> argv;
    for (const auto& word : command) argv.push_back(expand(word.get<std::string>(), expansion));
    std::string program { argv.front() };
    if (!base::is_absolute_path(program)) {
        // Found where the step runs: a developer environment may put another version of a tool first.
        std::optional<std::string> pathList;
        if (environment) {
            const bool caseInsensitive { mcppls::os::FAMILY == mcppls::os::Family::windows };
            for (const auto& entry : *environment) {
                const std::string name { entry.substr(0, entry.find('=')) };
                if (caseInsensitive ? base::to_lower_ascii(name) == "path" : name == "PATH") pathList = entry.substr(entry.find('=') + 1);
            }
        }
        auto found = pathList ? mcppls::platform::env::find_executable(program, *pathList) : mcppls::platform::env::find_executable(program);
        if (!found) {
            say("prepare: {} is not on PATH", program);
            return false;
        }
        program = *found;
    }
    mcppls::platform::SpawnOptions options;
    options.program = program;
    options.arguments.assign(argv.begin() + 1, argv.end());
    options.workDirectory = workspace;
    options.environment = environment;
    auto result = mcppls::platform::run(std::move(options), std::chrono::minutes { 20 });
    if (!result || result->exitCode != 0 || result->timedOut) {
        say("prepare failed: {}", lsp::dump(command));
        if (result) say("{}\n{}", result->output, result->error);
        return false;
    }
    if (verbose) say("prepare: {}\n{}", lsp::dump(command), result->output);
    return true;
}

// A file's size and FNV-1a digest, read a block at a time: a build tree holds files far larger than a check needs to keep.
std::optional<std::string> digest(const std::string& path) {
    std::ifstream stream { std::filesystem::path { path }, std::ios::binary };
    if (!stream) return std::nullopt;
    std::uint64_t hash { 1469598103934665603ull };
    std::uint64_t size { 0 };
    std::vector<char> block(std::size_t { 1 } << 16);
    while (stream.read(block.data(), static_cast<std::streamsize>(block.size())) || stream.gcount() > 0) {
        const auto count = static_cast<std::size_t>(stream.gcount());
        for (std::size_t i { 0 }; i < count; ++i) {
            hash ^= static_cast<unsigned char>(block[i]);
            hash *= 1099511628211ull;
        }
        size += count;
    }
    return std::format("{}:{:016x}", size, hash);
}

// Every file under a directory with its digest, dot directories included: the server must not write any of them.
std::map<std::string, std::string> snapshot(const std::string& root) {
    std::map<std::string, std::string> files;
    std::vector<std::string> pending { root };
    while (!pending.empty()) {
        const std::string directory { pending.back() };
        pending.pop_back();
        for (const auto& entry : fs::list_directory(directory)) {
            if (fs::is_directory(entry)) {
                pending.push_back(entry);
            } else if (auto relative = base::relative_path(entry, root)) {
                if (auto content = digest(entry)) files[*relative] = std::move(*content);
            }
        }
    }
    return files;
}

// The engine's published module files for a module under a cache directory, with their stamps. clangd
// publishes <module>.pcm (a partition as <module>-<partition>.pcm) under a directory per source and
// command; the copies it hands to readers carry a timestamp in their names and are not included.
std::map<std::string, std::string> module_files(const std::string& cacheDirectory, std::string_view module) {
    std::string published { module };
    std::ranges::replace(published, ':', '-');
    published += ".pcm";
    std::map<std::string, std::string> files;
    if (cacheDirectory.empty()) return files;
    std::vector<std::string> pending { cacheDirectory };
    while (!pending.empty()) {
        const std::string directory { pending.back() };
        pending.pop_back();
        for (const auto& entry : fs::list_directory(directory)) {
            if (fs::is_directory(entry)) {
                pending.push_back(entry);
            } else if (base::file_name(entry) == published) {
                const auto stamp = fs::stamp(entry);
                files[entry] = stamp ? std::format("{}:{}", stamp->size, stamp->modified) : std::string {};
            }
        }
    }
    return files;
}

// The last lines of each log file the server wrote under its cache directory.
void print_server_log_tail(const std::string& cacheDirectory) {
    constexpr std::size_t LINES { 200 };
    const std::string directory { base::join_path(cacheDirectory, "logs") };
    auto files = fs::list_directory(directory);
    std::ranges::sort(files);   // the names carry their start time: the newest last
    // A run that reuses a cache directory finds every earlier run's log there too; the last few are this one's.
    constexpr std::size_t FILES { 3 };
    if (files.size() > FILES) files.erase(files.begin(), files.end() - static_cast<std::ptrdiff_t>(FILES));
    for (const auto& file : files) {
        const auto text = fs::read_file(file);
        if (!text) continue;
        const auto lines = base::split_lines(*text);
        const std::size_t from { lines.size() > LINES ? lines.size() - LINES : 0 };
        say("--- server log {} (last {} of {} lines)", base::file_name(file), lines.size() - from, lines.size());
        for (std::size_t i { from }; i < lines.size(); ++i) say("  | {}", lines[i]);
    }
}

class Client {
private:
    std::unique_ptr<lsp::Connection> connection_;
    std::shared_ptr<mcppls::platform::Channel<Json>> inbox_ { std::make_shared<mcppls::platform::Channel<Json>>() };
    std::int64_t nextId_ { 1 };
    int unanswered_ { 0 }; // consecutive requests that reached their deadline
    bool verbose_ { false };

public:
    std::map<std::string, Json> diagnostics;     // uri -> latest diagnostics
    std::map<std::string, int> diagnosticsCount; // uri -> publishes received
    std::vector<std::string> progressKinds;      // $/progress kinds in order: begin, report…, end
    Json status;                                 // the latest cxxModules/status, whichever root sent it
    // usable plan W9.1: a multi-root session sends one cxxModules/status per root, each naming its
    // own project.root; `status` alone cannot tell them apart, so every root's latest is kept too.
    std::map<std::string, Json> statusByRoot;    // project.root (a DocumentUri) -> latest status
    std::vector<std::string> statusHistory;
    std::map<std::string, std::vector<std::string>> statusHistoryByRoot;   // project.root -> its states, in order
    // Timestamped, for the stress check's timeline (conformance/README.md): the longest interval
    // without progress while the project is not ready, and the state a run settled on.
    std::vector<std::pair<Clock::time_point, std::string>> statusTimeline;
    std::vector<Clock::time_point> progressTimes;
    std::optional<Clock::time_point> firstReady;         // the first status in state ready
    // Watchers the server registered through client/registerCapability, by registration id: the
    // runner reports its own writes to them the way an editor's file system watcher would.
    std::map<std::string, Json> watchers;
    std::optional<Clock::time_point> firstDiagnostics;   // the first diagnostics published once the server is ready or degraded

    base::Result<void> start(const Options& options, const std::vector<std::string>& serverArguments, const std::string& workspace,
                             const std::string& cacheDirectory) {
        verbose_ = options.verbose;
        mcppls::platform::SpawnOptions spawn;
        spawn.program = options.server;
        spawn.arguments = { "serve" };
        if (!options.payload.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--payload", options.payload });
        if (!options.clangd.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--clangd", options.clangd });
        if (!options.kit.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--kit", options.kit });
        spawn.arguments.insert(spawn.arguments.end(), serverArguments.begin(), serverArguments.end());
        spawn.workDirectory = workspace;
        auto environment = mcppls::platform::env::variables();
        environment.push_back("MCPPLS_CACHE_DIR=" + cacheDirectory);
        apply_isolated_home(environment, options);
        spawn.environment = std::move(environment);
        const bool verbose { verbose_ };
        auto inbox = inbox_;
        auto connection = lsp::Connection::start(
            std::move(spawn), [inbox, verbose](Json message) {
                if (verbose) std::cerr << "  <<< " << lsp::dump(message).substr(0, 120) << std::endl;
                inbox->push(std::move(message));
            }, [inbox] { inbox->close(); },
            [verbose](std::string_view line) {
                if (verbose) say("  server: {}", line);
            });
        if (!connection) return std::unexpected { connection.error() };
        connection_ = std::move(*connection);
        return {};
    }

    void notify(std::string_view method, Json params) { (void)connection_->send(lsp::make_notification(method, std::move(params))); }

    // A response with enough detail for the stress check to tell a real error apart from a real,
    // empty answer — both of which `request` below collapses to `Json(nullptr)`, which is fine for
    // every check that only asks "did it answer", but not for one that counts errors on their own.
    struct RequestOutcome {
        Json result { nullptr };
        bool timedOut { false };
        bool isError { false };
    };

    RequestOutcome request_full(std::string_view method, Json params, std::chrono::seconds timeout) {
        const std::int64_t id { nextId_++ };
        (void)connection_->send(lsp::make_request(id, method, std::move(params)));
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            auto message = inbox_->pop_until(deadline);
            if (!message) break;
            if (lsp::kind_of(*message) == lsp::Kind::response && (*message)["id"] == Json(id)) {
                unanswered_ = 0;
                if (message->contains("error")) {
                    if (verbose_) say("  error response to {}: {}", method, lsp::dump((*message)["error"]));
                    return { Json(nullptr), false, true };
                }
                return { message->value("result", Json {}), false, false };
            }
            dispatch(*message);
        }
        if (!inbox_->closed()) ++unanswered_;
        return { Json(nullptr), true, false };
    }

    std::optional<Json> request(std::string_view method, Json params, std::chrono::seconds timeout) {
        auto outcome = request_full(method, std::move(params), timeout);
        if (outcome.timedOut) return std::nullopt;
        return outcome.result;
    }

    // The peer's OS process id, for the stress check's process-tree CPU/RSS sampler. nullopt
    // wherever the platform cannot say (Windows, or no server started yet).
    std::optional<std::int64_t> native_pid() const { return connection_ ? connection_->native_pid() : std::nullopt; }

    // Why the remaining checks cannot run, or empty while the server is usable:
    // a server that exited, or one that let two requests in a row reach their
    // deadline, would only make every later check wait out its own.
    std::string unusable() {
        if (inbox_->closed() && inbox_->size() == 0) {
            const auto code = connection_->exit_code();
            return code ? std::format("the server exited with status {}", *code) : std::string { "the server closed its output" };
        }
        if (unanswered_ >= 2) return std::format("the server answered none of the last {} requests", unanswered_);
        return {};
    }

    // Processes incoming messages until `done` holds or the deadline passes.
    bool wait_for(const std::function<bool()>& done, std::chrono::seconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (!done()) {
            if (Clock::now() >= deadline) return false;
            auto message = inbox_->pop_until(std::min(deadline, Clock::now() + std::chrono::milliseconds { 200 }));
            if (message) dispatch(*message);
            else if (inbox_->closed() && inbox_->size() == 0) return done();
        }
        return true;
    }

    // Whether a registered watcher covers `path` for a change of `type` (1 created, 2 changed, 3 deleted).
    bool watches(std::string_view path, int type) const {
        for (const auto& [id, list] : watchers) {
            for (const auto& watcher : list) {
                if (!watcher.is_object() || !watcher.contains("globPattern")) continue;
                if ((watcher.value("kind", 7) & (1 << (type - 1))) == 0) continue;
                const Json& pattern = watcher["globPattern"];
                if (pattern.is_string()) {
                    if (base::glob_match(pattern.get<std::string>(), path)) return true;
                    continue;
                }
                if (!pattern.is_object() || !pattern.contains("pattern") || !pattern.contains("baseUri")) continue;
                const Json& baseUri = pattern["baseUri"];   // a URI, or a WorkspaceFolder
                const std::string uri { baseUri.is_string() ? baseUri.get<std::string>() : baseUri.value("uri", std::string {}) };
                const auto base = base::uri_to_path(uri);
                if (!base) continue;
                const auto relative = base::relative_path(path, *base);
                if (relative && base::glob_match(pattern.value("pattern", std::string {}), *relative)) return true;
            }
        }
        return false;
    }

    void drain(std::chrono::milliseconds quiet) {
        while (auto message = inbox_->pop_until(Clock::now() + quiet)) dispatch(*message);
    }

    void dispatch(const Json& message) {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::request: {
            const std::string method { message.value("method", std::string {}) };
            Json result = nullptr;
            if (method == "workspace/configuration") {
                result = Json::array();
                for (std::size_t i { 0 }; i < message["params"].value("items", Json::array()).size(); ++i) result.push_back(nullptr);
            } else if (method == "client/registerCapability") {
                for (const auto& registration : message["params"].value("registrations", Json::array())) {
                    if (registration.value("method", std::string {}) != "workspace/didChangeWatchedFiles") continue;
                    watchers[registration.value("id", std::string {})] = registration.value("registerOptions", Json::object()).value("watchers", Json::array());
                }
            } else if (method == "client/unregisterCapability") {
                // The protocol spells the field "unregisterations".
                for (const auto& registration : message["params"].value("unregisterations", Json::array())) {
                    watchers.erase(registration.value("id", std::string {}));
                }
            }
            (void)connection_->send(lsp::make_result(message["id"], std::move(result)));
            break;
        }
        case lsp::Kind::notification: {
            const std::string method { message.value("method", std::string {}) };
            if (method == "textDocument/publishDiagnostics") {
                const std::string uri { message["params"].value("uri", std::string {}) };
                diagnostics[uri] = message["params"].value("diagnostics", Json::array());
                ++diagnosticsCount[uri];
                const std::string state { status.is_object() ? status.value("state", std::string {}) : std::string {} };
                if (!firstDiagnostics && (state == "ready" || state == "degraded")) firstDiagnostics = Clock::now();
            } else if (method == "cxxModules/status") {
                status = message["params"];
                const std::string statusRoot { status.value("project", Json::object()).value("root", std::string {}) };
                statusByRoot[statusRoot] = status;
                statusHistory.push_back(status.value("state", std::string {}));
                statusHistoryByRoot[statusRoot].push_back(statusHistory.back());
                statusTimeline.emplace_back(Clock::now(), statusHistory.back());
                if (!firstReady && statusHistory.back() == "ready") firstReady = Clock::now();
                if (verbose_) say("  status: {}", lsp::dump(status));
            } else if (method == "$/progress") {
                // Neither `params` nor `value` is guaranteed to be an object: a forwarded engine
                // notification can carry anything, and nlohmann's `value()` throws on an array
                // rather than returning the default. Measured — it dumped core on the first run.
                const Json* params { lsp::find(message, "params") };
                const Json* value { params != nullptr && params->is_object() ? lsp::find(*params, "value") : nullptr };
                if (value != nullptr && value->is_object()) {
                    progressKinds.push_back(value->value("kind", std::string {}));
                    progressTimes.push_back(Clock::now());
                    if (verbose_) say("  progress: {} {}", progressKinds.back(), value->value("message", std::string {}));
                }
            } else if (verbose_ && method == "window/logMessage") {
                say("  log: {}", message["params"].value("message", std::string {}));
            }
            break;
        }
        default: break;
        }
    }

    void stop() {
        if (!connection_) return;
        (void)request("shutdown", nullptr, std::chrono::seconds { 10 });
        notify("exit", nullptr);
        connection_->stop(std::chrono::seconds { 5 });
    }
};

// An MCP client of `mcppls mcp` (S5 6): JSON-RPC messages one per line.
class McpClient {
private:
    std::unique_ptr<lsp::Connection> connection_;
    std::shared_ptr<mcppls::platform::Channel<Json>> inbox_ { std::make_shared<mcppls::platform::Channel<Json>>() };
    std::int64_t nextId_ { 1 };

public:
    base::Result<void> start(const Options& options, const std::vector<std::string>& serverArguments, const std::string& workspace,
                             const std::string& cacheDirectory, bool daemon) {
        mcppls::platform::SpawnOptions spawn;
        spawn.program = options.server;
        spawn.arguments = { "mcp", "--root", workspace };
        if (daemon) spawn.arguments.push_back("--daemon");
        if (!options.payload.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--payload", options.payload });
        if (!options.clangd.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--clangd", options.clangd });
        if (!options.kit.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--kit", options.kit });
        spawn.arguments.insert(spawn.arguments.end(), serverArguments.begin(), serverArguments.end());
        spawn.workDirectory = workspace;
        auto environment = mcppls::platform::env::variables();
        environment.push_back("MCPPLS_CACHE_DIR=" + cacheDirectory);
        apply_isolated_home(environment, options);
        spawn.environment = std::move(environment);
        const bool verbose { options.verbose };
        auto inbox = inbox_;
        auto connection = lsp::Connection::start(
            std::move(spawn), [inbox](Json message) { inbox->push(std::move(message)); }, [inbox] { inbox->close(); },
            [verbose](std::string_view line) {
                if (verbose) say("  mcp: {}", line);
            },
            lsp::Framing::lines);
        if (!connection) return std::unexpected { connection.error() };
        connection_ = std::move(*connection);
        const auto initialized = request("initialize", Json { { "protocolVersion", "2025-06-18" }, { "capabilities", Json::object() },
                                                              { "clientInfo", Json { { "name", "mcppls-conformance" }, { "version", std::string { base::VERSION } } } } },
                                         std::chrono::seconds { 60 }, {});
        if (!initialized || !initialized->contains("result")) return base::fail("mcp-initialize", "mcppls mcp did not answer initialize");
        (void)connection_->send(Json { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } });
        return {};
    }

    // The whole response, or nullopt; `idle` runs while waiting, so the language server's own output keeps being read.
    std::optional<Json> request(std::string_view method, Json params, std::chrono::seconds timeout, const std::function<void()>& idle) {
        const std::int64_t id { nextId_++ };
        if (!connection_->send(Json { { "jsonrpc", "2.0" }, { "id", id }, { "method", std::string { method } }, { "params", std::move(params) } })) return std::nullopt;
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            auto message = inbox_->pop_until(std::min(deadline, Clock::now() + std::chrono::milliseconds { 200 }));
            if (message && message->value("id", Json {}) == Json(id)) return message;
            if (!message && inbox_->closed() && inbox_->size() == 0) return std::nullopt;
            if (idle) idle();
        }
        return std::nullopt;
    }

    void stop() {
        if (connection_) connection_->stop(std::chrono::seconds { 10 });
    }
};

// The values a JSON pointer names; a "*" segment names every element of an array or member of an object.
void select(const Json& value, std::span<const std::string> segments, std::vector<const Json*>& out) {
    if (segments.empty()) {
        out.push_back(&value);
        return;
    }
    const std::string& segment { segments.front() };
    const auto rest = segments.subspan(1);
    if (segment == "*") {
        if (value.is_array() || value.is_object()) {
            for (const auto& item : value) select(item, rest, out);
        }
        return;
    }
    if (value.is_object()) {
        if (const auto found = value.find(segment); found != value.end()) select(*found, rest, out);
    } else if (value.is_array() && !segment.empty() && std::ranges::all_of(segment, [](char c) { return c >= '0' && c <= '9'; })) {
        const std::size_t index { static_cast<std::size_t>(std::stoul(segment)) };
        if (index < value.size()) select(value[index], rest, out);
    }
}

std::vector<const Json*> select(const Json& value, std::string_view pointer) {
    std::vector<std::string> segments;
    for (auto segment : base::split(pointer, '/')) segments.emplace_back(segment);
    if (!segments.empty() && segments.front().empty()) segments.erase(segments.begin());
    std::vector<const Json*> out;
    select(value, segments, out);
    return out;
}

// Whether `candidate` has every member `expected` has, recursively (arrays and scalars compare equal).
bool includes(const Json& candidate, const Json& expected) {
    if (!expected.is_object()) return candidate == expected;
    if (!candidate.is_object()) return false;
    return std::ranges::all_of(expected.items(), [&](const auto& item) { return candidate.contains(item.key()) && includes(candidate[item.key()], item.value()); });
}

// A fixture's expectations of a JSON result (conformance/README.md, S5 checks): each names a pointer and
// one of equals, contains, min-items, max-items, exists or absent, and holds when any value the pointer names satisfies it --
// except each-contains, which every value the pointer names must satisfy (and holds when it names none).
std::pair<bool, std::string> expectations_hold(const Json& value, const Json& expectations) {
    for (const auto& expectation : expectations) {
        const std::string pointer { expectation.value("path", std::string {}) };
        const auto matches = select(value, pointer);
        bool held { false };
        if (expectation.contains("absent")) {
            held = matches.empty();
        } else if (expectation.contains("each-contains")) {
            const std::string wanted { expectation.value("each-contains", std::string {}) };
            held = std::ranges::all_of(matches, [&](const Json* match) { return match->is_string() && match->get<std::string>().find(wanted) != std::string::npos; });
        } else if (expectation.contains("exists")) {
            held = !matches.empty();
        } else if (expectation.contains("equals")) {
            held = std::ranges::any_of(matches, [&](const Json* match) { return *match == expectation["equals"]; });
        } else if (expectation.contains("contains")) {
            const Json& wanted = expectation["contains"];
            held = std::ranges::any_of(matches, [&](const Json* match) {
                if (match->is_string() && wanted.is_string()) return match->get<std::string>().find(wanted.get<std::string>()) != std::string::npos;
                if (match->is_array()) return std::ranges::any_of(*match, [&](const Json& item) { return includes(item, wanted); });
                return false;
            });
        } else if (expectation.contains("min-items")) {
            const std::size_t wanted { expectation.value("min-items", std::size_t { 1 }) };
            held = std::ranges::any_of(matches, [&](const Json* match) { return (match->is_array() || match->is_object()) && match->size() >= wanted; });
        } else if (expectation.contains("max-items")) {
            const std::size_t wanted { expectation.value("max-items", std::size_t { 0 }) };
            held = std::ranges::any_of(matches, [&](const Json* match) { return (match->is_array() || match->is_object()) && match->size() <= wanted; });
        }
        if (!held) {
            std::string found { matches.empty() ? std::string { "nothing" } : lsp::dump(*matches.front()) };
            if (found.size() > 160) found = found.substr(0, 160) + "...";
            return { false, std::format("{} does not hold ({} found at {})", lsp::dump(expectation), found, pointer) };
        }
    }
    return { true, {} };
}

std::string state_of(const Json& status) { return status.is_object() ? status.value("state", std::string {}) : std::string {}; }

std::vector<std::string> location_uris(const Json& result) {
    std::vector<std::string> uris;
    auto add = [&](const Json& location) {
        if (!location.is_object()) return;
        if (location.contains("targetUri")) uris.push_back(location.value("targetUri", std::string {}));
        else if (location.contains("uri")) uris.push_back(location.value("uri", std::string {}));
    };
    if (result.is_array()) {
        for (const auto& location : result) add(location);
    } else {
        add(result);
    }
    return uris;
}

std::string hover_text(const Json& result) {
    if (!result.is_object()) return {};
    const Json contents = result.value("contents", Json {});
    if (contents.is_string()) return contents.get<std::string>();
    if (contents.is_object()) return contents.value("value", std::string {});
    std::string text;
    if (contents.is_array()) {
        for (const auto& item : contents) text += item.is_string() ? item.get<std::string>() : item.value("value", std::string {});
    }
    return text;
}

std::vector<std::string> completion_labels(const Json& result) {
    std::vector<std::string> labels;
    const Json items = result.is_object() ? result.value("items", Json::array()) : result;
    if (!items.is_array()) return labels;
    for (const auto& item : items) labels.push_back(std::string { base::trim(item.value("label", std::string {})) });
    return labels;
}

bool ends_with_path(std::string_view uri, std::string_view suffix) {
    auto path = base::uri_to_path(uri);
    if (!path) return false;
    return base::path_key(*path).ends_with(base::path_key(base::normalize_path(suffix)));
}

Json position(const Json& at) { return Json { { "line", at.at(0) }, { "character", at.at(1) } }; }

// ---- stress: seeded random use, per method answered/empty/timeout/error and latency ----------

// Every file under a directory, relative to it, '/'-separated: what a fixture's own glob
// ("src/**/*.cppm") is matched against, the same way S2's own watch globs are (base::glob_match).
std::vector<std::string> list_all_files(const std::string& root) {
    std::vector<std::string> files;
    std::vector<std::string> pending { root };
    while (!pending.empty()) {
        const std::string directory { pending.back() };
        pending.pop_back();
        for (const auto& entry : fs::list_directory(directory)) {
            if (fs::is_directory(entry)) pending.push_back(entry);
            else if (auto relative = base::relative_path(entry, root)) files.push_back(*relative);
        }
    }
    return files;
}

struct MethodStats {
    int answered { 0 };
    int empty { 0 };
    int timeout { 0 };
    int error { 0 };
    std::vector<double> latencies;   // seconds; answered and empty both answered in time
};

// Whether a well-formed, non-error result carries nothing: a legitimate "no information here"
// answer (null hover, an empty list) rather than a fault, so it is not counted as an error.
bool is_empty_result(std::string_view method, const Json& value) {
    if (value.is_null()) return true;
    if (method == "textDocument/hover") return hover_text(value).empty();
    if (method == "textDocument/completion") return completion_labels(value).empty();
    if (method == "textDocument/documentSymbol") return value.is_array() && value.empty();
    return location_uris(value).empty();   // definition, declaration, references
}

double percentile(std::vector<double> sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    const auto index = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::min(sorted.size(), std::max<std::size_t>(1, index)) - 1];
}

struct Spot {
    std::size_t line;
    std::size_t character;   // where to ask: the middle of the identifier
    std::size_t finish;      // one past its last character, for completion
};

// A random identifier in `text`, skipping comments, preprocessor lines and a short list of
// keywords too common to be interesting. Up to 50 random lines are tried before giving up.
std::optional<Spot> random_identifier(const std::string& text, std::mt19937_64& rng) {
    static const std::set<std::string_view> KEYWORDS {
        "const", "auto", "return", "if", "for", "while", "std", "int", "void", "bool", "class",
        "struct", "namespace", "import", "export", "module", "using", "public", "private", "case",
        "switch", "else", "do", "new", "delete", "this", "true", "false", "nullptr", "static",
        "constexpr", "template", "typename", "co_await", "co_return",
    };
    const auto lines = base::split_lines(text);
    if (lines.empty()) return std::nullopt;
    std::uniform_int_distribution<std::size_t> lineDist(0, lines.size() - 1);
    for (int attempt { 0 }; attempt < 50; ++attempt) {
        const std::size_t at { lineDist(rng) };
        const std::string_view line { lines[at] };
        const auto trimmed = base::trim(line);
        if (trimmed.starts_with("//") || trimmed.starts_with('#')) continue;
        std::vector<Spot> spots;
        std::size_t i { 0 };
        while (i < line.size()) {
            const unsigned char c { static_cast<unsigned char>(line[i]) };
            if (std::isalpha(c) || c == '_') {
                const std::size_t start { i };
                while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) ++i;
                const std::string_view word { line.substr(start, i - start) };
                if (word.size() > 2 && !KEYWORDS.contains(word)) spots.push_back({ at, start + word.size() / 2, i });
            } else {
                ++i;
            }
        }
        if (!spots.empty()) return spots[std::uniform_int_distribution<std::size_t>(0, spots.size() - 1)(rng)];
    }
    return std::nullopt;
}

// What the stress check's process-tree sampler measures: `getrusage(RUSAGE_CHILDREN)`-style
// accounting after the server exits is what design 1 asks for, but nothing this codebase already
// imports exposes it (openkal's process handle is opaque). Where the OS process id is known
// (Process::native_pid, POSIX only) and this host is Linux, the tree's CPU and RSS are sampled
// from /proc while the actions run instead: a per-pid high-water mark of CPU ticks, summed across
// every pid ever seen in the tree (so an exited helper's cost is not lost), and a high-water mark
// of the tree's total resident memory. Everywhere else this reports null, never fails the check.
class ProcessSampler {
private:
    std::jthread thread_;
    std::mutex mutex_;
    std::map<std::int64_t, long> cpuTicksByPid_;   // pid -> highest utime+stime ever seen
    double peakRssKB_ { 0 };
    bool active_ { false };
    // The near-universal Linux default (CLK_TCK=100 on every mainstream distribution this project
    // targets); a wrong guess only skews cpuSeconds, which stays a best-effort number either way.
    static constexpr long TICKS_PER_SECOND { 100 };

    static std::optional<std::int64_t> parent_of(std::int64_t pid) {
        auto stat = fs::read_file(std::format("/proc/{}/stat", pid));
        if (!stat) return std::nullopt;
        const auto close = stat->rfind(')');
        if (close == std::string::npos || close + 2 >= stat->size()) return std::nullopt;
        std::istringstream rest { stat->substr(close + 2) };
        char state {};
        std::int64_t ppid { -1 };
        rest >> state >> ppid;
        return rest.fail() ? std::nullopt : std::optional<std::int64_t> { ppid };
    }

    static std::optional<long> cpu_ticks_of(std::int64_t pid) {
        auto stat = fs::read_file(std::format("/proc/{}/stat", pid));
        if (!stat) return std::nullopt;
        const auto close = stat->rfind(')');
        if (close == std::string::npos || close + 2 >= stat->size()) return std::nullopt;
        std::istringstream rest { stat->substr(close + 2) };
        std::vector<std::string> fields;
        for (std::string field; rest >> field;) fields.push_back(field);
        if (fields.size() < 13) return std::nullopt;   // state=0 ... utime=11, stime=12
        try {
            return std::stol(fields[11]) + std::stol(fields[12]);
        } catch (...) {
            return std::nullopt;
        }
    }

    static std::optional<double> rss_kb_of(std::int64_t pid) {
        auto status = fs::read_file(std::format("/proc/{}/status", pid));
        if (!status) return std::nullopt;
        for (auto line : base::split_lines(*status)) {
            if (!line.starts_with("VmRSS:")) continue;
            std::istringstream rest { std::string { line.substr(6) } };
            double kb { 0 };
            rest >> kb;
            return rest.fail() ? std::nullopt : std::optional<double> { kb };
        }
        return std::nullopt;
    }

    // `root` and everything descended from it, by one pass over every numeric /proc entry
    // building a ppid map, then closing over it: the same approach stress.sh took with `ps`.
    static std::vector<std::int64_t> tree_pids(std::int64_t root) {
        std::map<std::int64_t, std::int64_t> parent;
        for (const auto& entry : fs::list_directory("/proc")) {
            const std::string name { base::file_name(entry) };
            if (name.empty() || !std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; })) continue;
            std::int64_t pid { 0 };
            try {
                pid = std::stoll(name);
            } catch (...) {
                continue;
            }
            if (auto ppid = parent_of(pid)) parent[pid] = *ppid;
        }
        std::set<std::int64_t> tree { root };
        for (bool changed { true }; changed;) {
            changed = false;
            for (const auto& [pid, ppid] : parent) {
                if (tree.contains(ppid) && tree.insert(pid).second) changed = true;
            }
        }
        return { tree.begin(), tree.end() };
    }

    void sample_once(std::int64_t root) {
        const auto pids = tree_pids(root);
        double rss { 0 };
        std::lock_guard lock { mutex_ };
        for (const auto pid : pids) {
            if (auto ticks = cpu_ticks_of(pid)) {
                auto& best = cpuTicksByPid_[pid];
                best = std::max(best, *ticks);
            }
            if (auto kb = rss_kb_of(pid)) rss += *kb;
        }
        peakRssKB_ = std::max(peakRssKB_, rss);
    }

public:
    // No-op wherever the pid or /proc are not available: `finish()` then reports null, as design 1
    // asks for anything a platform cannot measure.
    explicit ProcessSampler(std::optional<std::int64_t> rootPid) {
        if constexpr (mcppls::os::FAMILY != mcppls::os::Family::linux) {
            (void)rootPid;
            return;
        } else {
            if (!rootPid || !fs::is_directory("/proc")) return;
            active_ = true;
            const std::int64_t root { *rootPid };
            sample_once(root);
            thread_ = std::jthread { [this, root](std::stop_token token) {
                while (!token.stop_requested()) {
                    sample_once(root);
                    std::this_thread::sleep_for(std::chrono::milliseconds { 500 });
                }
                sample_once(root);
            } };
        }
    }

    struct Usage {
        std::optional<double> cpuSeconds;
        std::optional<double> peakRssMB;
    };

    Usage finish() {
        if (!active_) return {};
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
        std::lock_guard lock { mutex_ };
        long ticks { 0 };
        for (const auto& [pid, best] : cpuTicksByPid_) ticks += best;
        return { static_cast<double>(ticks) / static_cast<double>(TICKS_PER_SECOND), peakRssKB_ / 1024.0 };
    }
};

class Scenario {
private:
    Client& client_;
    const Options& options_;
    std::vector<std::string> serverArguments_;
    std::string workspace_;
    std::map<std::string, std::pair<std::string, int>> open_;   // relative path -> (text, version)
    std::chrono::seconds timeout_;
    std::map<std::string, std::string> prepared_;               // the workspace as the prepare steps left it
    std::string cacheDirectory_;                                // the server's cache
    bool expectWarm_ { false };
    std::map<std::string, std::map<std::string, std::string>> moduleFilesBefore_;   // module -> its published files before the server started
    std::unique_ptr<McpClient> mcp_;                            // started by the first mcp check
    std::unique_ptr<McpClient> mcpDaemon_;                      // the first mcp check "via": "daemon"
    std::string mcpFailure_;

    McpClient* mcp_client(bool daemon) {
        auto& kept = daemon ? mcpDaemon_ : mcp_;
        if (kept || !mcpFailure_.empty()) return kept.get();
        auto client = std::make_unique<McpClient>();
        // The agent's own server beside the editor's: it shares the cache directory as a guest (overall design 6.3);
        // through the daemon, a relay to the workspace's one warm session.
        if (auto started = client->start(options_, serverArguments_, workspace_, cacheDirectory_, daemon); !started) {
            mcpFailure_ = started.error().message;
            return nullptr;
        }
        kept = std::move(client);
        return kept.get();
    }

public:
    void finish() {
        if (mcp_) mcp_->stop();
        if (mcpDaemon_) mcpDaemon_->stop();
    }

    Scenario(Client& client, const Options& options, std::vector<std::string> serverArguments, std::string workspace, std::chrono::seconds timeout,
             std::map<std::string, std::string> prepared, std::string cacheDirectory, bool expectWarm,
             std::map<std::string, std::map<std::string, std::string>> moduleFilesBefore)
        : client_ { client }, options_ { options }, serverArguments_ { std::move(serverArguments) }, workspace_ { std::move(workspace) }, timeout_ { timeout }, prepared_ { std::move(prepared) },
          cacheDirectory_ { std::move(cacheDirectory) }, expectWarm_ { expectWarm }, moduleFilesBefore_ { std::move(moduleFilesBefore) } {}

    std::string uri(std::string_view relative) const { return base::path_to_uri(base::join_path(workspace_, relative)); }

    std::string text_of(std::string_view relative) {
        if (auto it = open_.find(std::string { relative }); it != open_.end()) return it->second.first;
        return fs::read_file(base::join_path(workspace_, relative)).value_or("");
    }

    void open(std::string_view relative, std::optional<std::string> text = {}) {
        const std::string content { text ? *text : text_of(relative) };
        if (open_.contains(std::string { relative })) {
            change(relative, content);
            return;
        }
        open_[std::string { relative }] = { content, 1 };
        client_.notify("textDocument/didOpen", Json { { "textDocument", Json { { "uri", uri(relative) }, { "languageId", "cpp" }, { "version", 1 }, { "text", content } } } });
    }

    void change(std::string_view relative, const std::string& text) {
        auto& [current, version] = open_[std::string { relative }];
        current = text;
        ++version;
        client_.notify("textDocument/didChange", Json { { "textDocument", Json { { "uri", uri(relative) }, { "version", version } } },
                                                        { "contentChanges", Json::array({ Json { { "text", text } } }) } });
    }

    // Repeats a request until `accept` holds, because the engine may still be preparing modules.
    std::pair<bool, Json> retry(std::string_view method, const std::function<Json()>& params, const std::function<bool(const Json&)>& accept) {
        const auto deadline = Clock::now() + timeout_;
        Json last;
        while (Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
            auto result = client_.request(method, params(), std::max(std::chrono::seconds { 1 }, remaining));
            if (result) {
                last = *result;
                if (accept(*result)) return { true, last };
            }
            client_.drain(std::chrono::milliseconds { 500 });
        }
        return { false, last };
    }

    std::pair<bool, std::string> run(const Json& check) {
        // A check may bound its own wait below the run's --timeout.
        const std::chrono::seconds runTimeout { timeout_ };
        if (auto own = check.find("timeout"); own != check.end() && own->is_number()) {
            timeout_ = std::min(runTimeout, std::chrono::seconds { own->get<std::int64_t>() });
        }
        auto result = run_(check);
        timeout_ = runTimeout;
        return result;
    }

    std::pair<bool, std::string> run_(const Json& check) {
        const std::string kind { check.value("kind", std::string {}) };
        const std::string file { check.value("file", std::string { "src/main.cpp" }) };
        // A check may bring its own unsaved buffer.
        if (auto text = check.find("text"); text != check.end()) open(file, text->get<std::string>());
        if (kind == "status") {
            // usable plan W9.1: "folder" selects one root's own status in a multi-root fixture
            // (relative to the fixture root, like a check's own "file"); absent, this checks
            // whatever cxxModules/status arrived most recently, the way a single-root fixture,
            // which only ever gets the one root's, always has.
            const auto folder = check.find("folder");
            const std::string rootUri { folder != check.end() ? uri(folder->get<std::string>()) : std::string {} };
            auto current = [&]() -> Json {
                if (rootUri.empty()) return client_.status;
                const auto it = client_.statusByRoot.find(rootUri);
                return it == client_.statusByRoot.end() ? Json {} : it->second;
            };
            // usable plan W9.4: error is as settled a state as ready or degraded (a corrupt
            // payload, for instance, does not become anything else once reported).
            const auto settled = [](const Json& status) {
                const std::string state { state_of(status) };
                return state == "ready" || state == "degraded" || state == "error";
            };
            const auto matches = [&](const Json& snapshot) {
                bool matched { settled(snapshot) };
                if (auto source = check.find("source"); source != check.end()) {
                    matched = matched && snapshot.value("project", Json::object()).value("source", std::string {}) == source->get<std::string>();
                }
                if (auto profile = check.find("profile-kind"); profile != check.end()) {
                    matched = matched && snapshot.value("profile", Json::object()).value("kind", std::string {}) == profile->get<std::string>();
                }
                if (auto state = check.find("state"); state != check.end()) matched = matched && state_of(snapshot) == state->get<std::string>();
                if (auto level = check.find("level"); level != check.end()) {
                    matched = matched && snapshot.value("project", Json::object()).value("level", 0) == level->get<int>();
                }
                // real-project plan RP3.2: `project.tier`, the README's L1..L4, distinct from `level`.
                if (auto tier = check.find("tier"); tier != check.end()) {
                    matched = matched && snapshot.value("project", Json::object()).value("tier", 0) == tier->get<int>();
                }
                if (auto issueCode = check.find("issue-code"); issueCode != check.end()) {
                    const std::string wantedCommand { check.value("issue-command", std::string {}) };
                    const std::string wantedMessage { check.value("issue-message", std::string {}) };   // a part of the message
                    const std::string wantedCategory { check.value("issue-category", std::string {}) };   // S3: code | engine | environment | project
                    matched = matched && std::ranges::any_of(snapshot.value("issues", Json::array()), [&](const Json& issue) {
                        if (issue.value("code", std::string {}) != issueCode->get<std::string>()) return false;
                        if (!wantedMessage.empty() && !issue.value("message", std::string {}).contains(wantedMessage)) return false;
                        if (!wantedCategory.empty() && issue.value("category", std::string {}) != wantedCategory) return false;
                        return wantedCommand.empty() || issue.value("command", Json::object()).value("command", std::string {}) == wantedCommand;
                    });
                }
                if (auto compiler = check.find("profile-compiler"); compiler != check.end()) {
                    matched = matched && snapshot.value("profile", Json::object()).value("compiler", std::string {}).starts_with(compiler->get<std::string>());
                }
                // overall design 5.2 and 5.6: the core engine, and every engine serving the root.
                if (auto engineName = check.find("engine-name"); engineName != check.end()) {
                    matched = matched && snapshot.value("engine", Json::object()).value("name", std::string {}) == engineName->get<std::string>();
                }
                if (auto engines = check.find("engines-include"); engines != check.end()) {
                    for (const auto& wanted : *engines) {
                        matched = matched && std::ranges::any_of(snapshot.value("engines", Json::array()), [&](const Json& engine) {
                            return engine.value("name", std::string {}) == wanted.get<std::string>();
                        });
                    }
                }
                if (auto noticeCode = check.find("notice-code"); noticeCode != check.end()) {
                    matched = matched && std::ranges::any_of(snapshot.value("notices", Json::array()), [&](const Json& notice) {
                        return notice.value("code", std::string {}) == noticeCode->get<std::string>();
                    });
                }
                return matched;
            };
            (void)client_.wait_for([&] { return settled(current()); }, timeout_);
            // A server coalesces changes that keep the state (S3 4), so the rest of a settled
            // status may follow a moment later: a mismatch gets a few seconds more, not the whole timeout.
            if (!matches(current())) (void)client_.wait_for([&] { return matches(current()); }, std::min(timeout_, std::chrono::seconds { 3 }));
            const Json snapshot = current();   // `Json x { y }` would wrap y in a one-element array; `=` copies it
            return { matches(snapshot), lsp::dump(snapshot) };
        }
        if (kind == "second-instance") {
            // overall design 6.3: another server on the same workspace and cache, as an agent's own
            // server beside an editor's, reports that it keeps a private cache.
            Client second;
            if (auto started = second.start(options_, serverArguments_, workspace_, cacheDirectory_); !started) return { false, started.error().message };
            Json capabilities { { "experimental", Json { { "cxxModules", Json { { "version", 1 }, { "status", true } } } } } };
            auto initialized = second.request("initialize", Json { { "processId", nullptr }, { "rootUri", base::path_to_uri(workspace_) },
                { "workspaceFolders", Json::array({ Json { { "uri", base::path_to_uri(workspace_) }, { "name", "second" } } }) },
                { "capabilities", capabilities } }, std::chrono::seconds { 60 });
            if (!initialized || !initialized->is_object()) {
                second.stop();
                return { false, "the second server did not answer initialize" };
            }
            second.notify("initialized", Json::object());
            const std::string wanted { check.value("notice-code", std::string { "shared-workspace" }) };
            const auto hasNotice = [&] {
                if (!second.status.is_object()) return false;
                return std::ranges::any_of(second.status.value("notices", Json::array()), [&](const Json& notice) {
                    return notice.is_object() && notice.value("code", std::string {}) == wanted;
                });
            };
            const bool found { second.wait_for(hasNotice, timeout_) };
            const Json snapshot = second.status;
            second.stop();
            return { found, lsp::dump(snapshot) };
        }
        if (kind == "mcp") {
            // S5 6: a tool call (or, with "method", any request) to `mcppls mcp`, repeated until its result
            // meets the expectations or the check's time is up; "is-error" expects a tool error instead.
            McpClient* mcp { mcp_client(check.value("via", std::string {}) == "daemon") };
            if (mcp == nullptr) return { false, "cannot start mcppls mcp: " + mcpFailure_ };
            const std::string method { check.value("method", std::string { "tools/call" }) };
            const Json params = method == "tools/call" ? Json { { "name", check.value("tool", std::string {}) }, { "arguments", check.value("arguments", Json::object()) } }
                                                       : check.value("params", Json::object());
            const bool expectError { check.value("is-error", false) };
            const auto deadline = Clock::now() + timeout_;
            std::string why { "no answer" };
            do {
                const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
                const auto response = mcp->request(method, params, std::max(std::chrono::seconds { 1 }, remaining), [this] { client_.drain(std::chrono::milliseconds { 0 }); });
                if (!response) break;
                Json value = response->value("result", Json {});
                bool isError { false };
                if (method == "tools/call") {
                    isError = value.value("isError", false);
                    const Json content = value.value("content", Json::array());
                    value = value.contains("structuredContent") ? value["structuredContent"]
                                                                : Json::parse(content.empty() ? std::string { "null" } : content[0].value("text", std::string { "null" }), nullptr, false);
                }
                auto [held, detail] = expectations_hold(value, check.value("expect", Json::array()));
                if (held && isError == expectError) return { true, lsp::dump(value).substr(0, 160) };
                why = isError != expectError ? std::format("isError is {}: {}", isError, lsp::dump(value).substr(0, 300)) : detail;
                if (!check.value("retry", true)) break;
                client_.drain(std::chrono::milliseconds { 1000 });
            } while (Clock::now() < deadline);
            return { false, why };
        }
        if (kind == "cli") {
            // S5 7: a command of the query entries, run to completion in the workspace; its standard output is
            // one JSON document meeting the expectations, and it exits with "exit" (0 unless given).
            mcppls::platform::SpawnOptions spawn;
            spawn.program = options_.server;
            for (const auto& argument : check.value("args", Json::array())) spawn.arguments.push_back(argument.get<std::string>());
            if (!options_.payload.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--payload", options_.payload });
            if (!options_.clangd.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--clangd", options_.clangd });
            if (!options_.kit.empty()) spawn.arguments.insert(spawn.arguments.end(), { "--kit", options_.kit });
            spawn.arguments.insert(spawn.arguments.end(), serverArguments_.begin(), serverArguments_.end());
            spawn.workDirectory = workspace_;
            auto environment = mcppls::platform::env::variables();
            environment.push_back("MCPPLS_CACHE_DIR=" + cacheDirectory_);
            apply_isolated_home(environment, options_);
            spawn.environment = std::move(environment);
            auto running = std::async(std::launch::async, [spawn, timeout = timeout_]() mutable { return mcppls::platform::run(std::move(spawn), timeout); });
            while (running.wait_for(std::chrono::milliseconds { 200 }) != std::future_status::ready) client_.drain(std::chrono::milliseconds { 0 });
            auto result = running.get();
            if (!result) return { false, result.error().message };
            if (result->timedOut) return { false, "the command did not finish in time" };
            const int wantedExit { check.value("exit", 0) };
            if (result->exitCode != wantedExit) return { false, std::format("exit {} instead of {}: {}", result->exitCode, wantedExit, (result->output + result->error).substr(0, 300)) };
            const Json value = Json::parse(result->output, nullptr, false);
            if (value.is_discarded()) return { false, "the output is not JSON: " + result->output.substr(0, 200) };
            auto [held, detail] = expectations_hold(value, check.value("expect", Json::array()));
            return { held, held ? lsp::dump(value).substr(0, 160) : detail };
        }
        if (kind == "type-text") {
            // import-hang plan §8: a person typing a line one key at a time. Line `line` of `file` takes each of
            // `steps` in turn, `interval-ms` apart (the whole buffer is sent, as editors with full sync do); after each,
            // `request` (default documentSymbol) must be answered within `answer-within` seconds. With `save`, each step
            // is also written to disk and reported as saved and changed, as autosave does. The check fails when the
            // status turned to any state of `states-never` meanwhile.
            open(file);
            const int line { check.value("line", 0) };
            const std::chrono::milliseconds interval { check.value("interval-ms", 150) };
            const std::string method { check.value("request", std::string { "textDocument/documentSymbol" }) };
            const std::chrono::seconds answerWithin { check.value("answer-within", 5) };
            const bool save { check.value("save", false) };
            const auto startedAt = Clock::now();
            const std::size_t timelineBefore { client_.statusTimeline.size() };
            double worst { 0 };
            for (const auto& step : check.value("steps", Json::array())) {
                const std::string current { text_of(file) };
                std::vector<std::string> lines;
                for (const auto each : base::split_lines(current)) lines.emplace_back(each);
                if (line < 0 || static_cast<std::size_t>(line) >= lines.size()) return { false, std::format("line {} is not in {}", line, file) };
                lines[static_cast<std::size_t>(line)] = step.get<std::string>();
                std::string text;
                for (const auto& each : lines) text += each + "\n";
                change(file, text);
                if (save) {
                    (void)fs::write_file_atomic(base::join_path(workspace_, file), text);
                    client_.notify("textDocument/didSave", Json { { "textDocument", Json { { "uri", uri(file) } } } });
                    client_.notify("workspace/didChangeWatchedFiles", Json { { "changes", Json::array({ Json { { "uri", uri(file) }, { "type", 2 } } }) } });
                }
                const auto asked = Clock::now();
                Json params { { "textDocument", Json { { "uri", uri(file) } } } };
                if (method != "textDocument/documentSymbol" && method != "textDocument/semanticTokens/full") {
                    params["position"] = Json { { "line", line }, { "character", 0 } };
                }
                if (!client_.request(method, params, answerWithin)) {
                    return { false, std::format("{} was not answered within {} s after the line became '{}'", method, answerWithin.count(), step.get<std::string>()) };
                }
                worst = std::max(worst, std::chrono::duration<double>(Clock::now() - asked).count());
                client_.drain(interval);
            }
            for (std::size_t i { timelineBefore }; i < client_.statusTimeline.size(); ++i) {
                const auto& [at, state] = client_.statusTimeline[i];
                for (const auto& never : check.value("states-never", Json::array())) {
                    if (state == never.get<std::string>()) {
                        return { false, std::format("the status turned {} {:.1f} s into the typing", state, std::chrono::duration<double>(at - startedAt).count()) };
                    }
                }
            }
            return { true, std::format("{} steps, slowest answer {:.2f} s", check.value("steps", Json::array()).size(), worst) };
        }
        if (kind == "clangd-check") {
            // import-hang plan §9, a workaround's canary: the runner's own clangd (--clangd, else the payload's) is run with
            // --check on `file`; `expect` is "hangs" (it has not finished after `seconds`, default 10) or "finishes". A canary
            // expects the defect its workaround exists for; once an update of clangd fixes it, the check fails with `says`.
            const std::string clangd { !options_.clangd.empty() ? options_.clangd
                                                                : base::join_path(options_.payload, "clangd/bin/clangd") + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
            if (!fs::is_regular_file(clangd)) return { false, "no clangd: pass --clangd or --payload" };
            mcppls::platform::SpawnOptions spawn;
            spawn.program = clangd;
            spawn.arguments = { std::format("--check={}", base::join_path(workspace_, file)), "--check-tidy-time=0" };
            spawn.workDirectory = workspace_;
            const std::chrono::seconds limit { check.value("seconds", 10) };
            auto running = std::async(std::launch::async, [spawn, limit]() mutable { return mcppls::platform::run(std::move(spawn), limit); });
            while (running.wait_for(std::chrono::milliseconds { 200 }) != std::future_status::ready) client_.drain(std::chrono::milliseconds { 0 });
            auto result = running.get();
            if (!result) return { false, result.error().message };
            const bool hung { result->timedOut };
            const std::string expected { check.value("expect", std::string { "hangs" }) };
            const std::string says { check.value("says", std::string {}) };
            if (expected == "hangs" && !hung) {
                return { false, std::format("{} (clangd exited {} in under {} s: {})", says.empty() ? std::string { "clangd finished; the defect is gone" } : says,
                                            result->exitCode, limit.count(), (result->output + result->error).substr(0, 200)) };
            }
            if (expected == "finishes" && hung) return { false, std::format("clangd did not finish {} in {} s", file, limit.count()) };
            return { true, hung ? std::format("clangd has not finished {} after {} s, as expected", file, limit.count()) : "clangd finished" };
        }
        if (kind == "responds") {
            // An answer of any kind, an empty one included, within the check's time: a file the engine
            // cannot serve must be answered at once rather than left waiting (usable plan W1.7, W5.4).
            open(file);
            const std::string method { check.value("method", std::string { "textDocument/definition" }) };
            const auto started = Clock::now();
            const auto answer = client_.request(method, Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } },
                                                timeout_);
            const double seconds { std::chrono::duration<double>(Clock::now() - started).count() };
            return { answer.has_value(), answer ? std::format("{:.1f}s: {}", seconds, lsp::dump(*answer).substr(0, 120)) : std::string { "no answer" } };
        }
        if (kind == "model-origin") {
            // Build description design 4.1: a cold run plans with what the producer says, a warm one
            // plans with the cached model at once and confirms it in the background. The report says
            // which it was, so the difference is checked rather than assumed.
            const std::string expected { expectWarm_ ? check.value("warm", std::string { "cache-fresh" })
                                                     : check.value("cold", std::string { "producer" }) };
            const auto deadline = Clock::now() + timeout_;
            std::string found { "no answer" };
            do {
                const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
                auto answer = client_.request("cxxModules/report", Json::object(), std::max(std::chrono::seconds { 1 }, remaining));
                if (!answer) break;
                const Json& roots { (*answer)["roots"] };
                // Where the model this session STARTED with came from. `origin` moves on as the
                // producer confirms what the cache said; what this check is about is the start.
                const Json* origin { roots.is_array() && !roots.empty() ? lsp::find_path(roots.front(), { "project", "firstOrigin" }) : nullptr };
                found = origin != nullptr && origin->is_string() ? origin->get<std::string>() : std::string { "absent" };
                if (found == expected) return { true, found };
                client_.drain(std::chrono::milliseconds { 500 });
            } while (Clock::now() < deadline);
            return { false, std::format("the model came from {}, not {}", found, expected) };
        }
        if (kind == "module-cache-reused") {
            // SC4: a warm start builds no module the previous run left in the cache. Every published
            // file of the module is still there unchanged, and none was added under another command.
            const std::string module { check.value("module", std::string { "std" }) };
            const auto before = moduleFilesBefore_.find(module);
            if (before == moduleFilesBefore_.end() || before->second.empty()) {
                return { !expectWarm_, std::format("no module file of {} before the server started: a cold start", module) };
            }
            const auto now = module_files(cacheDirectory_, module);
            std::vector<std::string> differences;
            for (const auto& [path, stamp] : before->second) {
                const auto it = now.find(path);
                if (it == now.end()) differences.push_back("removed " + path);
                else if (it->second != stamp) differences.push_back("rebuilt " + path);
            }
            for (const auto& [path, stamp] : now) {
                if (!before->second.contains(path)) differences.push_back("added " + path);
            }
            return { differences.empty(), differences.empty() ? std::format("{} file(s) of {} reused", now.size(), module) : lsp::dump(differences) };
        }
        if (kind == "workspace-unchanged") {
            // Give the server time to do what it does after opening the workspace.
            client_.drain(std::chrono::milliseconds { 2000 });
            const auto now = snapshot(workspace_);
            std::vector<std::string> differences;
            for (const auto& [path, content] : now) {
                const auto before = prepared_.find(path);
                if (before == prepared_.end()) differences.push_back("added " + path);
                else if (before->second != content) differences.push_back("changed " + path);
            }
            for (const auto& [path, content] : prepared_) {
                if (!now.contains(path)) differences.push_back("removed " + path);
            }
            if (differences.size() > 8) differences.resize(8);
            return { differences.empty(), lsp::dump(differences) };
        }
        if (kind == "open") {
            open(file);
            return { true, file };
        }
        if (kind == "write-file") {
            // usable plan W9.3: writes a file directly, the way an editor's own file system watcher
            // (or, without one, this server's own polling fallback) would notice it, without the
            // runner opening it as a document. `content` defaults to a fresh module interface.
            // `content-from` names a workspace file to copy instead, for content too long to spell out.
            std::string content { check.value("content", std::format("export module {};\n", check.value("module", std::string { "probe" }))) };
            if (const std::string from { check.value("content-from", std::string {}) }; !from.empty()) {
                auto copied = fs::read_file(base::join_path(workspace_, from));
                if (!copied) return { false, std::format("{}: {}", from, copied.error().message) };
                content = std::move(*copied);
            }
            const std::string path { base::join_path(workspace_, file) };
            (void)fs::create_directories(base::parent_path(path));
            // With "folder", the reload must be that root's own (usable plan W9.1): a change routed
            // to another root would reload that one instead.
            const std::string folderUri { check.contains("folder") ? uri(check.value("folder", std::string {})) : std::string {} };
            const auto history = [&]() -> const std::vector<std::string>& {
                return folderUri.empty() ? client_.statusHistory : client_.statusHistoryByRoot[folderUri];
            };
            const std::size_t seen { history().size() };
            const bool existed { fs::exists(path) };
            const auto written = fs::write_file(path, content);
            if (!written) return { false, written.error().message };
            // An editor reports the write to the watchers the server registered; with nothing
            // registered (--no-dynamic-watch) the server's own polling has to notice it.
            const int type { existed ? 2 : 1 };
            const std::string canonical { fs::canonical_path(path) };
            if (client_.watches(path, type) || client_.watches(canonical, type)) {
                client_.notify("workspace/didChangeWatchedFiles", Json { { "changes", Json::array({ Json { { "uri", base::path_to_uri(path) }, { "type", type } } }) } });
            }
            if (!check.value("expect-reload", false)) return { true, file };
            // S2 5: a change to an input the producer named loads the model again.
            const bool reloaded { client_.wait_for([&] {
                const auto& states = history();
                return states.size() > seen && std::ranges::find(states.begin() + static_cast<std::ptrdiff_t>(seen), states.end(), "loading") != states.end();
            }, timeout_) };
            return { reloaded, reloaded ? std::format("{}: the model loaded again", file) : std::format("{}: no reload", file) };
        }
        if (kind == "diagnostics-empty") {
            open(file);
            const std::string documentUri { uri(file) };
            const bool published { client_.wait_for([&] {
                return client_.diagnosticsCount[documentUri] > 0 && state_of(client_.status) != "preparing" && state_of(client_.status) != "loading";
            }, timeout_) };
            client_.drain(std::chrono::milliseconds { 1500 });
            Json errors = Json::array();
            for (const auto& diagnostic : client_.diagnostics[documentUri]) {
                if (diagnostic.value("severity", 1) == 1) errors.push_back(diagnostic.value("message", std::string {}));
            }
            return { published && errors.empty(), published ? lsp::dump(errors) : std::string { "no diagnostics were published" } };
        }
        if (kind == "execute-command") {
            // overall design 7.7: a command the server declared, answered without an error.
            const auto answer = client_.request("workspace/executeCommand",
                                                Json { { "command", check.value("command", std::string {}) }, { "arguments", check.value("arguments", Json::array()) } },
                                                timeout_);
            return { answer.has_value(), answer ? lsp::dump(*answer) : std::string { "no answer, or an error" } };
        }
        if (kind == "diagnostic-code") {
            open(file);
            const std::string documentUri { uri(file) };
            const std::string code { check.value("expect", std::string {}) };
            const bool found { client_.wait_for([&] {
                for (const auto& diagnostic : client_.diagnostics[documentUri]) {
                    if (diagnostic.value("code", Json {}) == Json(code)) return true;
                }
                return false;
            }, timeout_) };
            return { found, lsp::dump(client_.diagnostics[documentUri]) };
        }
        if (kind == "definition" || kind == "definition-any" || kind == "declaration") {
            open(file);
            const std::string expected { check.value("expect", std::string {}) };
            auto [ok, result] = retry(kind == "declaration" ? "textDocument/declaration" : "textDocument/definition",
                [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } }; },
                [&](const Json& value) {
                    const auto uris = location_uris(value);
                    if (kind == "definition-any") return !uris.empty();
                    return std::ranges::any_of(uris, [&](const std::string& found) { return ends_with_path(found, expected); });
                });
            return { ok, lsp::dump(location_uris(result)) };
        }
        if (kind == "hover-contains") {
            open(file);
            // `expect` is one text the hover must contain, or several of which any one will do -- for a
            // check that accepts either an engine's answer or the server's own explanation of why it
            // cannot answer yet.
            std::vector<std::string> expected;
            if (const auto it = check.find("expect"); it != check.end() && it->is_array()) {
                for (const auto& one : *it) expected.push_back(one.get<std::string>());
            } else {
                expected.push_back(check.value("expect", std::string {}));
            }
            auto [ok, result] = retry("textDocument/hover",
                [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } }; },
                [&](const Json& value) {
                    const std::string text { hover_text(value) };
                    return std::ranges::any_of(expected, [&](const std::string& one) { return text.find(one) != std::string::npos; });
                });
            std::string text { hover_text(result) };
            return { ok, text.substr(0, std::min<std::size_t>(text.size(), 160)) };
        }
        if (kind == "completion-contains") {
            open(file);
            if (auto insert = check.find("insert"); insert != check.end()) {
                // [line, text]: the text is inserted as a new line before `line`.
                // The views split_lines returns point into `current`, which must outlive them.
                const std::string current { text_of(file) };
                const auto lines = base::split_lines(current);
                std::vector<std::string> copy { lines.begin(), lines.end() };
                const std::size_t at { std::min<std::size_t>(insert->at(0).get<std::size_t>(), copy.size()) };
                copy.insert(copy.begin() + static_cast<std::ptrdiff_t>(at), insert->at(1).get<std::string>());
                change(file, base::join(copy, "\n") + "\n");
            }
            if (auto edit = check.find("edit"); edit != check.end()) {
                // {"file": "...", "replace": "...", "with": "..."}: an unsaved edit in another open buffer.
                const std::string other { edit->value("file", std::string {}) };
                std::string content { text_of(other) };
                content = base::replace_all(content, edit->value("replace", std::string {}), edit->value("with", std::string {}));
                open(other, content);
                client_.drain(std::chrono::milliseconds { 1000 });
                // Touch the importing buffer so it is rebuilt against the edited module.
                change(file, text_of(file) + " ");
            }
            const std::string expected { check.value("expect", std::string {}) };
            auto [ok, result] = retry("textDocument/completion",
                [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } }; },
                [&](const Json& value) {
                    const auto labels = completion_labels(value);
                    return std::ranges::any_of(labels, [&](const std::string& label) { return label.starts_with(expected); });
                });
            auto labels = completion_labels(result);
            if (labels.size() > 12) labels.resize(12);
            return { ok, lsp::dump(labels) };
        }
        if (kind == "references-span") {
            open(file);
            std::vector<std::string> expected;
            for (const auto& item : check.value("expect", Json::array())) expected.push_back(item.get<std::string>());
            auto [ok, result] = retry("textDocument/references",
                [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) },
                                    { "context", Json { { "includeDeclaration", true } } } }; },
                [&](const Json& value) {
                    const auto uris = location_uris(value);
                    return std::ranges::all_of(expected, [&](const std::string& path) {
                        return std::ranges::any_of(uris, [&](const std::string& found) { return ends_with_path(found, path); });
                    });
                });
            std::set<std::string> files;
            for (const auto& found : location_uris(result)) files.insert(std::string { base::file_name(found) });
            return { ok, lsp::dump(files) };
        }
        if (kind == "document-symbol-contains") {
            open(file);
            const std::string expected { check.value("expect", std::string {}) };
            auto [ok, result] = retry("textDocument/documentSymbol", [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } } }; },
                [&](const Json& value) {
                    if (!value.is_array()) return false;
                    return std::ranges::any_of(value, [&](const Json& symbol) { return symbol.value("name", std::string {}) == expected; });
                });
            return { ok, lsp::dump(result).substr(0, 160) };
        }
        if (kind == "report") {
            // robustness design O3: cxxModules/report, held to "expect" like a tool's result, retried within the check's time
            // (a plan or an engine may still be on its way).
            const auto deadline = Clock::now() + timeout_;
            std::string why { "no answer" };
            do {
                const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
                auto answer = client_.request("cxxModules/report", Json::object(), std::max(std::chrono::seconds { 1 }, remaining));
                if (!answer) break;
                auto [held, detail] = expectations_hold(*answer, check.value("expect", Json::array()));
                if (held) return { true, lsp::dump(answer->value("roots", Json::array())).substr(0, 160) };
                why = detail;
                client_.drain(std::chrono::milliseconds { 500 });
            } while (Clock::now() < deadline);
            return { false, why };
        }
        if (kind == "set-context") {
            // usable plan W9.2: cxxModules/setContext (S3 5.4), then a hover that should have
            // changed once the engine reloads under the new context's arguments.
            open(file);
            const std::string context { check.value("context", std::string {}) };
            auto set = client_.request("cxxModules/setContext",
                Json { { "textDocument", Json { { "uri", uri(file) } } }, { "context", context } }, timeout_);
            if (!set) return { false, std::format("no response to setContext({})", context) };
            const std::string expected { check.value("expect", std::string {}) };
            auto [ok, result] = retry("textDocument/hover",
                [&] { return Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } }; },
                [&](const Json& value) { return hover_text(value).find(expected) != std::string::npos; });
            std::string text { hover_text(result) };
            return { ok, text.substr(0, std::min<std::size_t>(text.size(), 160)) };
        }
        if (kind == "module-graph-contains") {
            // Retries within the check's own timeout (a check's "timeout" field, e.g. usable plan
            // W9.3's watch-polling fixture, bounds how long a change may take to reach the graph).
            const std::string expected { check.value("expect", std::string {}) };
            auto [ok, result] = retry("cxxModules/graph", [] { return Json::object(); }, [&](const Json& value) {
                if (!value.is_object()) return false;
                return std::ranges::any_of(value.value("modules", Json::array()),
                    [&](const Json& module) { return module.value("name", std::string {}) == expected; });
            });
            return { ok, result.is_object() ? lsp::dump(result).substr(0, 160) : std::string { "no response" } };
        }
        if (kind == "stress") {
            // Real-project stress testing (real-project plan RP0): seeded random use.
            // Files matching "files" are opened, some in quick succession without waiting for an
            // answer; at random identifier positions one of hover/definition/references/completion/
            // documentSymbol is asked. Per method: answered (a non-empty result), empty (a
            // well-formed but empty one), timeout (no response within "requestTimeout"), error, and
            // p50/p90/max latency. The status timeline's longest gap with no progress while not
            // ready, and the state it settled on. The server's process tree CPU seconds and peak
            // RSS, null wherever a platform cannot say. Deterministic for a given "seed".
            const std::uint64_t seed { static_cast<std::uint64_t>(check.value("seed", 1)) };
            const int actionCount { std::max(1, check.value("actions", 60)) };
            const std::chrono::seconds requestTimeout { check.value("requestTimeout", 10) };
            std::vector<std::string> patterns;
            for (const auto& item : check.value("files", Json::array({ "src/**/*.cppm", "src/**/*.cpp" }))) patterns.push_back(item.get<std::string>());
            std::vector<std::string> candidates;
            for (const auto& found : list_all_files(workspace_)) {
                if (std::ranges::any_of(patterns, [&](const std::string& pattern) { return base::glob_match(pattern, found); })) candidates.push_back(found);
            }
            if (candidates.empty()) return { false, "no file in the workspace matches the stress scenario's \"files\" globs" };

            std::mt19937_64 rng { seed };
            std::uniform_int_distribution<std::size_t> fileDist(0, candidates.size() - 1);
            static constexpr std::array<std::string_view, 5> METHODS { "textDocument/hover", "textDocument/definition",
                                                                        "textDocument/references", "textDocument/completion",
                                                                        "textDocument/documentSymbol" };
            std::uniform_int_distribution<std::size_t> methodDist(0, METHODS.size() - 1);
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            std::map<std::string, MethodStats> stats;

            const auto windowStart { Clock::now() };
            ProcessSampler sampler { client_.native_pid() };

            std::string currentFile;
            auto pick_and_open = [&] { currentFile = candidates[fileDist(rng)]; open(currentFile); };
            pick_and_open();
            client_.drain(std::chrono::milliseconds { 200 });

            for (int i { 0 }; i < actionCount; ++i) {
                const double roll { unit(rng) };
                if (roll < 0.25) {
                    pick_and_open();   // switch, and act at once
                } else if (roll < 0.30) {
                    for (int k { 0 }; k < 3; ++k) pick_and_open();   // fast switching: several without waiting
                }
                const std::string text { text_of(currentFile) };
                auto spot = random_identifier(text, rng);
                if (!spot) continue;
                const std::string method { std::string { METHODS[methodDist(rng)] } };
                Json params { { "textDocument", Json { { "uri", uri(currentFile) } } },
                             { "position", Json { { "line", spot->line }, { "character", spot->character } } } };
                if (method == "textDocument/references") params["context"] = Json { { "includeDeclaration", true } };
                else if (method == "textDocument/documentSymbol") params = Json { { "textDocument", Json { { "uri", uri(currentFile) } } } };
                else if (method == "textDocument/completion") params["position"]["character"] = spot->finish;
                const auto started { Clock::now() };
                auto outcome = client_.request_full(method, params, requestTimeout);
                const double elapsed { std::chrono::duration<double>(Clock::now() - started).count() };
                auto& s = stats[method];
                if (outcome.timedOut) ++s.timeout;
                else if (outcome.isError) { ++s.error; s.latencies.push_back(elapsed); }
                else if (is_empty_result(method, outcome.result)) { ++s.empty; s.latencies.push_back(elapsed); }
                else { ++s.answered; s.latencies.push_back(elapsed); }
            }
            const auto windowEnd { Clock::now() };
            const auto usage = sampler.finish();

            // The longest interval with no status change and no $/progress while not "ready".
            double maxStallSeconds { 0.0 };
            {
                std::vector<Clock::time_point> times { windowStart, windowEnd };
                std::vector<std::pair<Clock::time_point, std::string>> events;
                for (const auto& [at, state] : client_.statusTimeline) {
                    if (at >= windowStart && at <= windowEnd) events.emplace_back(at, state);
                }
                for (const auto at : client_.progressTimes) {
                    if (at >= windowStart && at <= windowEnd) times.push_back(at);
                }
                for (const auto& [at, state] : events) times.push_back(at);
                std::ranges::sort(times);
                times.erase(std::unique(times.begin(), times.end()), times.end());
                std::string state { state_of(client_.status) };
                // The state as of windowStart: the latest status strictly before it, if any.
                for (const auto& [at, seenState] : client_.statusTimeline) {
                    if (at <= windowStart) state = seenState;
                }
                std::size_t next { 0 };
                for (std::size_t i { 0 }; i + 1 < times.size(); ++i) {
                    while (next < events.size() && events[next].first <= times[i]) { state = events[next].second; ++next; }
                    if (state != "ready") maxStallSeconds = std::max(maxStallSeconds, std::chrono::duration<double>(times[i + 1] - times[i]).count());
                }
            }

            Json methods = Json::object();
            std::vector<double> allLatencies;
            int totalTimeouts { 0 };
            int totalErrors { 0 };
            for (auto& [method, s] : stats) {
                std::ranges::sort(s.latencies);
                methods[method] = Json { { "answered", s.answered }, { "empty", s.empty }, { "timeout", s.timeout }, { "error", s.error },
                                         { "p50", percentile(s.latencies, 0.5) }, { "p90", percentile(s.latencies, 0.9) },
                                         { "max", s.latencies.empty() ? 0.0 : s.latencies.back() } };
                allLatencies.insert(allLatencies.end(), s.latencies.begin(), s.latencies.end());
                totalTimeouts += s.timeout;
                totalErrors += s.error;
            }
            std::ranges::sort(allLatencies);
            const double p90 { percentile(allLatencies, 0.9) };
            const std::string finalState { state_of(client_.status) };
            const double windowMinutes { std::max(1.0 / 60.0, std::chrono::duration<double>(windowEnd - windowStart).count() / 60.0) };
            const std::optional<double> cpuPerMinute { usage.cpuSeconds ? std::optional<double> { *usage.cpuSeconds / windowMinutes } : std::nullopt };

            Json summary { { "seed", seed }, { "actions", actionCount }, { "files", candidates.size() }, { "methods", std::move(methods) },
                          { "timeouts", totalTimeouts }, { "errors", totalErrors }, { "p90", p90 }, { "maxStallSeconds", maxStallSeconds },
                          { "finalState", finalState },
                          { "cpuSeconds", usage.cpuSeconds ? Json(*usage.cpuSeconds) : Json(nullptr) },
                          { "cpuSecondsPerMinute", cpuPerMinute ? Json(*cpuPerMinute) : Json(nullptr) },
                          { "rssMB", usage.peakRssMB ? Json(*usage.peakRssMB) : Json(nullptr) } };

            std::vector<std::string> failures;
            if (const auto budget = check.find("budget"); budget != check.end() && budget->is_object()) {
                if (auto limit = budget->find("timeouts"); limit != budget->end() && totalTimeouts > limit->get<int>()) {
                    failures.push_back(std::format("{} timeout(s) over budget {}", totalTimeouts, limit->get<int>()));
                }
                if (auto limit = budget->find("p90"); limit != budget->end() && p90 > limit->get<double>()) {
                    failures.push_back(std::format("p90 {:.2f}s over budget {:.2f}s", p90, limit->get<double>()));
                }
                if (auto limit = budget->find("maxStallSeconds"); limit != budget->end() && maxStallSeconds > limit->get<double>()) {
                    failures.push_back(std::format("stall {:.1f}s over budget {:.1f}s", maxStallSeconds, limit->get<double>()));
                }
                if (auto limit = budget->find("cpuSecondsPerMinute"); limit != budget->end() && cpuPerMinute && *cpuPerMinute > limit->get<double>()) {
                    failures.push_back(std::format("{:.1f} CPU-second(s)/minute over budget {:.1f}", *cpuPerMinute, limit->get<double>()));
                }
                if (auto limit = budget->find("rssMB"); limit != budget->end() && usage.peakRssMB && *usage.peakRssMB > limit->get<double>()) {
                    failures.push_back(std::format("{:.0f}MB RSS over budget {:.0f}MB", *usage.peakRssMB, limit->get<double>()));
                }
            }
            return { failures.empty(), failures.empty() ? lsp::dump(summary) : std::format("{}: {}", base::join(failures, "; "), lsp::dump(summary)) };
        }
        return { false, std::format("unknown check kind {}", kind) };
    }
};

int run(Options options) {
    const std::string scenarioPath { base::join_path(options.fixture, "scenario.json") };
    auto scenarioText = fs::read_file(scenarioPath);
    if (!scenarioText) {
        say("conformance: {} not found", scenarioPath);
        return 2;
    }
    Json scenario = Json::parse(*scenarioText, nullptr, false);
    if (scenario.is_discarded()) {
        say("conformance: {} is not valid JSON", scenarioPath);
        return 2;
    }
    // --stress-seed: `mcppls-devtools stress --seed N` overriding whatever seed the fixture's own
    // stress checks carry, so a matrix run over several fixtures can still be reproduced exactly.
    if (options.stressSeed && scenario.contains("checks") && scenario["checks"].is_array()) {
        for (auto& check : scenario["checks"]) {
            if (check.is_object() && check.value("kind", std::string {}) == "stress") check["seed"] = *options.stressSeed;
        }
    }
    const std::string name { scenario.value("name", std::string { base::file_name(options.fixture) }) };
    const bool reused { !options.workspaceDirectory.empty() };
    const std::string scratch { reused ? options.workspaceDirectory
                                       : base::join_path(mcppls::platform::dirs::temp_directory(),
                                             std::format("mcppls-conformance-{}-{}", name, Clock::now().time_since_epoch().count())) };
    const std::string workspace { base::join_path(scratch, name) };
    // A reused workspace is prepared once; the marker sits beside it, outside what the server sees.
    const std::string preparedMarker { base::join_path(scratch, name + ".prepared") };
    const bool alreadyPrepared { reused && fs::exists(preparedMarker) };
    if (!alreadyPrepared) {
        fs::remove_all(workspace);
        copy_tree(options.fixture, workspace);
        fs::remove_all(base::join_path(workspace, "scenario.json"));
    }
    say("fixture {} in {}{}", name, workspace, alreadyPrepared ? " (prepared before)" : "");

    const std::string self { absolute(mcppls::platform::env::arguments().front()) };
    // real-project plan RP2.1: an isolated HOME so producer negotiation
    // (`mcppls::project::other_mcpp_executables`) sees only candidates this fixture put there,
    // never a real mcpp or xlings install on the host or CI runner running the fixture.
    std::string isolatedHome;
    if (scenario.value("isolate-home", false)) {
        isolatedHome = base::join_path(workspace, ".home");
        (void)fs::create_directories(isolatedHome);
        options.isolatedHome = isolatedHome;
    }
    Expansion expansion { workspace, base::parent_path(self), options.payload, self, isolatedHome };
    std::optional<std::vector<std::string>> prepareEnvironment;
    if (scenario.value("prepare-environment", std::string {}) == "msvc") {
        if (options.msvcEnvironment.empty()) {
            say("conformance: {} builds with MSVC; pass --msvc-env with a developer environment", name);
            return 2;
        }
        prepareEnvironment = prepare_environment(options.msvcEnvironment);
    }
    if (!alreadyPrepared) {
        for (const auto& command : scenario.value("prepare", Json::array())) {
            if (!run_prepare(command, workspace, options.verbose, expansion, prepareEnvironment)) return 1;
        }
        for (const auto& removed : scenario.value("remove", Json::array())) fs::remove_all(base::join_path(workspace, removed.get<std::string>()));
        if (reused) (void)fs::write_file(preparedMarker, "");
    }

    std::vector<std::string> serverArguments;
    for (const auto& argument : scenario.value("server-arguments", Json::array())) serverArguments.push_back(expand(argument.get<std::string>(), expansion));
    auto prepared = snapshot(workspace);
    Client client;
    const std::string cacheDirectory { options.cacheDirectory.empty() ? base::join_path(scratch, "cache") : options.cacheDirectory };
    std::map<std::string, std::map<std::string, std::string>> moduleFilesBefore;
    for (const auto& check : scenario.value("checks", Json::array())) {
        if (check.value("kind", std::string {}) != "module-cache-reused") continue;
        const std::string module { check.value("module", std::string { "std" }) };
        moduleFilesBefore[module] = module_files(cacheDirectory, module);
    }
    if (auto started = client.start(options, serverArguments, workspace, cacheDirectory); !started) {
        say("conformance: cannot start the server: {}", started.error().message);
        return 2;
    }
    const auto begin = Clock::now();
    Json capabilities {
        { "textDocument", Json { { "hover", Json { { "contentFormat", Json::array({ "markdown", "plaintext" }) } } },
                                 { "documentSymbol", Json { { "hierarchicalDocumentSymbolSupport", true } } },
                                 { "completion", Json { { "completionItem", Json { { "snippetSupport", false } } } } },
                                 { "publishDiagnostics", Json { { "relatedInformation", true } } } } },
        // usable plan W9.3: --no-dynamic-watch exercises the polling fallback the same way a
        // client with no didChangeWatchedFiles support would.
        { "workspace", Json { { "didChangeWatchedFiles", Json { { "dynamicRegistration", !options.noDynamicWatch }, { "relativePatternSupport", true } } },
                              { "configuration", true } } },
        { "window", Json { { "workDoneProgress", true } } },
    };
    // --client: the capabilities a real editor actually sends (none keeps this runner's own
    // long-standing default so every fixture written before this option existed is unaffected).
    Json initializationOptions = Json::object();
    switch (options.client) {
    case Options::ClientProfile::neovim:
        // editors/nvim/lua/mcppls/init.lua: only cxxModules.status, not graph or contexts.
        capabilities["experimental"] = Json { { "cxxModules", Json { { "version", 1 }, { "status", true } } } };
        initializationOptions["conflictArbitration"] = "client";
        break;
    case Options::ClientProfile::vscode:
        // editors/vscode/src/extension.ts: the full block, plus how it tells the server it
        // arbitrates language-feature conflicts with other C++ extensions itself.
        capabilities["experimental"] = Json { { "cxxModules", Json { { "version", 1 }, { "status", true }, { "graph", true }, { "contexts", true } } } };
        initializationOptions["conflictArbitration"] = "client";
        break;
    case Options::ClientProfile::zed:
    case Options::ClientProfile::plain:
        break;   // no experimental.cxxModules, no initializationOptions
    case Options::ClientProfile::none:
        if (!options.plainClient) {
            capabilities["experimental"] = Json { { "cxxModules", Json { { "version", 1 }, { "status", true },
                                                                         { "graph", true }, { "contexts", true } } } };
        }
        break;
    }
    // usable plan W9.1: a fixture with several roots names them, relative to the fixture's own
    // root, in "folders"; a check names a file or a folder the same way, relative to that root,
    // regardless of how many workspace folders the fixture actually declares.
    Json workspaceFolders = Json::array();
    if (const auto folders = scenario.find("folders"); folders != scenario.end() && folders->is_array() && !folders->empty()) {
        for (const auto& folder : *folders) {
            const std::string relative { folder.get<std::string>() };
            workspaceFolders.push_back(Json { { "uri", base::path_to_uri(base::join_path(workspace, relative)) }, { "name", relative } });
        }
    } else {
        workspaceFolders.push_back(Json { { "uri", base::path_to_uri(workspace) }, { "name", name } });
    }
    Json initializeParams { { "processId", nullptr }, { "rootUri", base::path_to_uri(workspace) },
                            { "workspaceFolders", workspaceFolders }, { "capabilities", capabilities } };
    if (!initializationOptions.empty()) initializeParams["initializationOptions"] = initializationOptions;
    auto initialized = client.request("initialize", std::move(initializeParams), std::chrono::seconds { 120 });
    if (!initialized || !initialized->is_object()) {
        say("FAIL initialize: no result");
        return 1;
    }
    // In plain-client mode the server is talked to the way every editor but this repository's own
    // VS Code extension talks to it, so `experimental.cxxModules` is neither sent nor expected.
    const bool plainLike { is_plain_like(options) };
    const bool advertised { plainLike
                            || initialized->contains("capabilities") && (*initialized)["capabilities"].contains("experimental")
                            && (*initialized)["capabilities"]["experimental"].contains("cxxModules") };
    const double initializeSeconds { std::chrono::duration<double>(Clock::now() - begin).count() };
    say("{} initialize ({:.1f}s) experimental.cxxModules={}{}", advertised ? "PASS" : "FAIL", initializeSeconds, advertised,
        plainLike ? " (plain client)" : "");
    client.notify("initialized", Json::object());

    Scenario runner { client, options, serverArguments, workspace, options.timeout, std::move(prepared), cacheDirectory, options.expectWarm, std::move(moduleFilesBefore) };
    int failures { advertised ? 0 : 1 };
    // "initialize-within": seconds. The handshake is answered at all, and in time (0.0.3 plan B1).
    if (const auto within = scenario.find("initialize-within"); within != scenario.end() && within->is_number()) {
        const bool inTime { initializeSeconds <= within->get<double>() };
        say("{} initialize within {}s ({:.1f}s)", inTime ? "PASS" : "FAIL", within->get<double>(), initializeSeconds);
        if (!inTime) ++failures;
    }
    Json measured = Json::array();
    for (const auto& check : scenario.value("checks", Json::array())) {
        const std::string id { check.value("id", std::string { "-" }) };
        const bool optional { check.value("optional", false) };
        // A `status` check reads `cxxModules/status`, which a plain client does not ask for and
        // must not receive. Running it in plain-client mode asserts the server breaks its own
        // contract; skipping it is the point, not a concession. Everything else still runs, which
        // is what makes this mode worth having: the standard surface must work without the
        // custom one.
        if (plainLike && check.value("kind", std::string {}) == "status") {
            say("SKIP {} status (a plain client asks for no cxxModules/status)", id);
            continue;
        }
        if (const auto reason = client.unusable(); !reason.empty()) {
            if (!optional) ++failures;
            say("{} {} {} (not run) {}", optional ? "SKIP" : "FAIL", id, check.value("kind", std::string {}), reason);
            continue;
        }
        const auto started = Clock::now();
        auto [ok, detail] = runner.run(check);
        if (!ok && !optional) ++failures;
        const double seconds { std::chrono::duration<double>(Clock::now() - started).count() };
        say("{} {} {} ({:.1f}s) {}", ok ? "PASS" : (optional ? "SKIP" : "FAIL"), id, check.value("kind", std::string {}), seconds, detail);
        measured.push_back(Json { { "id", id }, { "kind", check.value("kind", std::string {}) }, { "ok", ok }, { "seconds", seconds },
                                  { "since-start", std::chrono::duration<double>(Clock::now() - begin).count() }, { "detail", detail } });
    }
    runner.finish();
    client.stop();
    Json firstNavigation = nullptr;
    for (const auto& check : measured) {
        const std::string kind { check.value("kind", std::string {}) };
        if ((kind == "definition" || kind == "declaration" || kind == "definition-any") && check.value("ok", false)) {
            firstNavigation = check["since-start"];
            break;
        }
    }
    if (options.navigationBudget) {
        const bool within { firstNavigation.is_number() && firstNavigation.get<double>() <= *options.navigationBudget };
        if (!within) ++failures;
        say("{} navigation-budget first navigation {} within {:.1f}s", within ? "PASS" : "FAIL",
            firstNavigation.is_number() ? std::format("{:.2f}s", firstNavigation.get<double>()) : std::string { "never answered" }, *options.navigationBudget);
    }
    const double total { std::chrono::duration<double>(Clock::now() - begin).count() };
    // The point of plain-client mode: a client with no custom capability must still be told that
    // work is happening. Standard `$/progress` is the only channel it has, and before the
    // cold-start work it received nothing at all.
    if (plainLike) {
        const auto& kinds = client.progressKinds;
        const bool began { std::ranges::find(kinds, std::string { "begin" }) != kinds.end() };
        say("{} plain client receives $/progress ({} notification(s))", began ? "PASS" : "FAIL", kinds.size());
        if (!began) ++failures;
    }

    // A failure in CI leaves nothing behind but this output: the server's own log says what it was
    // doing while a check waited, which the check's one line cannot. --verbose already printed it.
    if (failures > 0 && !options.verbose) print_server_log_tail(cacheDirectory);
    say("{}: {} failure(s), {:.1f}s", name, failures, total);
    if (!options.measureFile.empty()) {
        // The timeline of usable plan W7: initialize, the first ready state, the first diagnostics, the first navigation.
        auto since = [&](const std::optional<Clock::time_point>& at) -> Json {
            return at ? Json(std::chrono::duration<double>(*at - begin).count()) : Json(nullptr);
        };
        Json summary { { "fixture", name }, { "failures", failures }, { "seconds", total }, { "reused-workspace", alreadyPrepared } };
        summary["initialize"] = initializeSeconds;
        summary["ready"] = since(client.firstReady);
        summary["first-diagnostics"] = since(client.firstDiagnostics);
        summary["first-navigation"] = firstNavigation;
        summary["checks"] = measured;
        if (auto written = fs::write_file(options.measureFile, summary.dump(2) + "\n"); !written) say("conformance: cannot write {}", options.measureFile);
    }
    if (!options.keep && !reused) fs::remove_all(scratch);
    return failures == 0 ? 0 : 1;
}

// ---- prepare: what a fixture generates before the server sees it ------------------------------
// A fixture's scenario.json names `["{conformance}", "prepare", "<kind>", ...]`; the step runs in the
// fixture's scratch workspace. These were Python scripts, and moved here so a conformance host needs
// nothing the runner does not already bring (tooling architecture §4): the runner prepares its own
// fixtures. Each writes what its script wrote -- same files, same JSON keys and values.

// A path as a tool on this host spells it: Windows compilers and their databases take backslashes.
std::string native(std::string path) {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) std::ranges::replace(path, '/', '\\');
    return path;
}

std::optional<std::string> on_path(const std::string& program) {
    if (base::is_absolute_path(program)) return program;
    return mcppls::platform::env::find_executable(program);
}

// usable plan W9.2: a workspace carrying its own S1 database with two sets that compile
// src/main.cpp with -DVARIANT=1 and -DVARIANT=2. No build system runs; this is what a producer
// would have written. The compiler is real (scenario.json passes {env:CONFORMANCE_CLANGXX|clang++}),
// so the server's toolchain probing at load time resolves it rather than guessing from a fake path.
int prepare_s1_two_sets(const std::string& compiler) {
    const std::string root { fs::current_directory() };
    auto clangxx = on_path(compiler.empty() ? std::string { "clang++" } : compiler);
    if (!clangxx) {
        say("s1-two-sets: {} is not on PATH", compiler);
        return 1;
    }
    const std::string source { native(base::join_path(root, "src/main.cpp")) };
    auto translation_unit = [&](int variant) {
        return Json {
            { "source", source },
            { "work-directory", native(root) },
            { "arguments", Json::array({ *clangxx, "-std=c++23", std::format("-DVARIANT={}", variant), "-c", source,
                                         "-o", native(base::join_path(root, std::format("main-{}.o", variant))) }) },
            { "local-arguments", Json::array({ std::format("-DVARIANT={}", variant) }) },
        };
    };
    auto set_for = [&](int variant) {
        return Json {
            { "name", std::format("variant{}", variant) },
            { "family-name", "probe" },
            { "visible-sets", Json::array() },
            { "baseline-arguments", Json::array({ "-std=c++23" }) },
            { "ide", { { "toolchain", "llvm-22.1.8" }, { "configuration", std::format("variant{}", variant) }, { "kind", "executable" } } },
            { "translation-units", Json::array({ translation_unit(variant) }) },
        };
    };
    const Json database {
        { "version", 1 },
        { "revision", 0 },
        { "ide", {
            { "profile-version", "0.2.0" },
            { "generator", { { "name", "mcppls-conformance" }, { "version", "0.0.0" } } },
            { "toolchains", { { "llvm-22.1.8", {
                { "family", "clang" }, { "version", "22.1.8" }, { "driver", *clangxx }, { "target", "x86_64-unknown-linux-gnu" },
            } } } },
        } },
        { "sets", Json::array({ set_for(1), set_for(2) }) },
    };
    if (auto written = fs::write_file(base::join_path(root, "build_database.json"), database.dump(2)); !written) {
        say("s1-two-sets: {}", written.error().message);
        return 1;
    }
    return 0;
}

// usable plan W9.4: a copy of the runner's own payload whose clangd no longer matches payload.json.
// The manifest's `files` entries for clangd and kit.json are filled in first, from the intact files,
// so the check is proven even against a payload assembled before W9.4. Linux only (the fixture list
// says so): the executable is `clangd/bin/clangd`.
int prepare_payload_corrupt(const std::string& source) {
    if (source.empty() || !fs::is_directory(source)) {
        say("payload-corrupt: pass the runner's --payload directory (the {{payload}} placeholder)");
        return 1;
    }
    const std::string target { base::join_path(fs::current_directory(), "payload-copy") };
    std::error_code failed;
    std::filesystem::remove_all(target, failed);
    std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, failed);
    if (failed) {
        say("payload-corrupt: cannot copy {}: {}", source, failed.message());
        return 1;
    }
    // A copy that is to be judged by the server must differ from the original in the one way the
    // fixture intends, so the execute bits the copy did not carry are put back.
    std::vector<std::string> executables;
    for (const auto& entry : std::filesystem::recursive_directory_iterator { source, failed }) {
        if (!entry.is_regular_file()) continue;
        if ((entry.status().permissions() & std::filesystem::perms::owner_exec) == std::filesystem::perms::none) continue;
        executables.push_back(base::join_path(target, std::filesystem::relative(entry.path(), source).generic_string()));
    }
    if (auto marked = fs::make_executable(executables); !marked) {
        say("payload-corrupt: {}", marked.error().message);
        return 1;
    }
    const std::string manifestPath { base::join_path(target, "payload.json") };
    auto text = fs::read_file(manifestPath);
    Json manifest = text ? Json::parse(*text, nullptr, false) : Json {};
    if (!manifest.is_object()) {
        say("payload-corrupt: {} is not a JSON object", manifestPath);
        return 1;
    }
    if (!manifest.contains("files")) manifest["files"] = Json::object();
    const std::string clangd { base::join_path(target, "clangd/bin/clangd") };
    for (const auto& [relative, path] : { std::pair { std::string { "clangd/bin/clangd" }, clangd },
                                          std::pair { std::string { "kit/kit.json" }, base::join_path(target, "kit/kit.json") } }) {
        if (manifest["files"].contains(relative) || !fs::is_regular_file(path)) continue;
        auto content = fs::read_file(path);
        if (!content) continue;
        manifest["files"][relative] = { { "size", content->size() }, { "sha256", base::sha256_hex(*content) } };
    }
    if (auto written = fs::write_file(manifestPath, manifest.dump(2)); !written) {
        say("payload-corrupt: {}", written.error().message);
        return 1;
    }
    // Corrupt it now, after the manifest above was computed from the still-intact file.
    auto intact = fs::read_file(clangd);
    if (!intact) {
        say("payload-corrupt: {} has no clangd/bin/clangd", source);
        return 1;
    }
    if (auto written = fs::write_file(clangd, std::string_view { *intact }.substr(0, std::min<std::size_t>(1024, intact->size()))); !written) {
        say("payload-corrupt: {}", written.error().message);
        return 1;
    }
    return 0;
}

// Builds the fixture as a build tool would, with the MSVC STL's std module, and writes
// compile_commands.json. Two drivers: `clang-cl`, which passes /clang: arguments after its inputs
// so an .ixx cannot be named a module interface and std is compiled from a .cppm copy of std.ixx
// (usable plan E7); and `clang++` for the MSVC ABI. Windows only, inside a developer environment
// (VCToolsInstallDir).
int prepare_compdb_msvc_std(bool clangCl) {
    const std::string root { fs::current_directory() };
    const std::string build { base::join_path(root, "build") };
    (void)fs::create_directories(build);
    const auto tools = mcppls::platform::env::get("VCToolsInstallDir");
    if (!tools) {
        say("prepare: VCToolsInstallDir is not set; run inside a developer environment (--msvc-env)");
        return 1;
    }
    const std::string stdIxx { base::join_path(*tools, "modules/std.ixx") };
    auto driver = mcppls::platform::env::find_executable(clangCl ? "clang-cl" : "clang++");
    if (!driver) {
        say("prepare: {} is not on PATH", clangCl ? "clang-cl" : "clang++");
        return 1;
    }
    auto at = [&](std::string_view directory, std::string_view name) { return native(base::join_path(directory, name)); };
    const std::string greet { at(root, "src/greet.cppm") };
    const std::string main { at(root, "src/main.cpp") };
    std::vector<std::pair<std::string, std::vector<std::string>>> steps;
    if (clangCl) {
        std::error_code failed;
        std::filesystem::copy_file(stdIxx, base::join_path(build, "std.cppm"), std::filesystem::copy_options::overwrite_existing, failed);
        if (failed) {
            say("prepare: cannot copy {}: {}", stdIxx, failed.message());
            return 1;
        }
        const std::vector<std::string> common { *driver, "/nologo", "/std:c++latest", "/EHsc", "/MD", "/c" };
        auto with = [&](std::vector<std::string> more) { auto all = common; all.insert(all.end(), more.begin(), more.end()); return all; };
        steps.emplace_back("", with({ "/clang:-Wno-reserved-module-identifier", "/clang:-Wno-include-angled-in-module-purview",
                                      "/clang:-fmodule-output=" + at(build, "std.pcm"), "/Fo" + at(build, "std.obj"), at(build, "std.cppm") }));
        steps.emplace_back(greet, with({ "/clang:-fmodule-output=" + at(build, "greet.pcm"), "/clang:-fmodule-file=std=" + at(build, "std.pcm"),
                                         "/Fo" + at(build, "greet.obj"), greet }));
        steps.emplace_back(main, with({ "/clang:-fmodule-file=std=" + at(build, "std.pcm"), "/clang:-fmodule-file=greet=" + at(build, "greet.pcm"),
                                        "/Fo" + at(build, "main.obj"), main }));
    } else {
        const std::vector<std::string> common { *driver, "--target=x86_64-pc-windows-msvc", "-std=c++23", "-c" };
        auto with = [&](std::vector<std::string> more) { auto all = common; all.insert(all.end(), more.begin(), more.end()); return all; };
        steps.emplace_back("", with({ "-Wno-reserved-module-identifier", "-Wno-include-angled-in-module-purview", "-x", "c++-module",
                                      native(stdIxx), "-fmodule-output=" + at(build, "std.pcm"), "-o", at(build, "std.obj") }));
        steps.emplace_back(greet, with({ "-fmodule-file=std=" + at(build, "std.pcm"), "-fmodule-output=" + at(build, "greet.pcm"),
                                         greet, "-o", at(build, "greet.obj") }));
        steps.emplace_back(main, with({ "-fmodule-file=std=" + at(build, "std.pcm"), "-fmodule-file=greet=" + at(build, "greet.pcm"),
                                        main, "-o", at(build, "main.obj") }));
    }
    Json database = Json::array();
    for (const auto& [source, argv] : steps) {
        say("+ {}", base::join(argv, " "));
        mcppls::platform::SpawnOptions options;
        options.program = argv.front();
        options.arguments.assign(argv.begin() + 1, argv.end());
        options.workDirectory = root;
        auto result = mcppls::platform::run(std::move(options), std::chrono::minutes { 10 });
        if (!result || result->exitCode != 0 || result->timedOut) {
            if (result) say("{}\n{}", result->output, result->error);
            say("prepare: the step above failed");
            return 1;
        }
        if (!source.empty()) database.push_back({ { "directory", native(root) }, { "file", source }, { "arguments", argv } });
    }
    if (auto written = fs::write_file(base::join_path(root, "compile_commands.json"), database.dump(2)); !written) {
        say("prepare: {}", written.error().message);
        return 1;
    }
    return 0;
}

// generated-module-old-mcpp: a plain compile_commands.json (no S1, no module-specific flags —
// what a project that has never seen mcpp's build database would already have) for the same three
// files as the generated-module fixture, real-compiled with `argument` (or clang++ on PATH), so
// the server's L2 fallback (mcpp advertises no mcpp.build-database, `build --configure-only` fails
// too, the project's own compile_commands.json is what is left) has something real to read.
int prepare_generated_module_compdb(const std::string& compiler) {
    const std::string root { fs::current_directory() };
    auto clangxx = on_path(compiler.empty() ? std::string { "clang++" } : compiler);
    if (!clangxx) {
        say("generated-module-old-mcpp: {} is not on PATH", compiler);
        return 1;
    }
    auto at = [&](std::string_view relative) { return native(base::join_path(root, relative)); };
    Json database = Json::array();
    for (const std::string_view relative : { "target/.build-mcpp/deps/xpkg@1.0.0/out/xpkg_lua_stdlib.cppm", "src/consumer.cppm", "src/main.cpp" }) {
        const std::string source { at(relative) };
        database.push_back(Json { { "directory", native(root) }, { "file", source },
                                  { "arguments", Json::array({ *clangxx, "-std=c++23", "-c", source, "-o", source + ".o" }) } });
    }
    if (auto written = fs::write_file(base::join_path(root, "compile_commands.json"), database.dump(2)); !written) {
        say("generated-module-old-mcpp: {}", written.error().message);
        return 1;
    }
    return 0;
}

// real-project plan RP2.1: a second, newer mock mcpp under a fixture's isolated HOME
// (`"isolate-home": true`), at the path producer negotiation searches
// (`mcppls::project::other_mcpp_executables`, `xim-x-mcpp/<version>/bin/mcpp`), so a fixture whose
// project mcpp cannot answer `emit build-database` (`mcpp-mock.json`'s `oldProtocol`) can prove
// negotiation finds and uses a working one instead of falling back to `compile_commands.json`. Its
// own `mcpp-mock.json`, beside it (mockmcpp reads a config beside its own executable when a
// fixture put one there, since a negotiated candidate still runs with the project's own directory
// as its cwd), describes the same generated-package/sibling-module project as generated-module,
// with the compiler this prepare step resolves itself: prepare steps run in the real environment,
// never the isolated one the server sees, so a path baked in now still exists once the server asks.
int prepare_producer_candidate(const std::string& home) {
    if (home.empty() || !fs::is_directory(home)) {
        say("producer-candidate: pass the fixture's isolated home (the {{home}} placeholder needs \"isolate-home\": true)");
        return 1;
    }
    const std::string root { fs::current_directory() };
    // As {env:CONFORMANCE_CLANGXX|clang++} expands in a scenario: an empty variable is an unset one
    // (CI sets it to "" where the runner's own clang++ is meant).
    const auto configured = mcppls::platform::env::get("CONFORMANCE_CLANGXX");
    auto clangxx = on_path(configured && !configured->empty() ? *configured : std::string { "clang++" });
    if (!clangxx) {
        say("producer-candidate: clang++ is not on PATH");
        return 1;
    }
    const std::string self { absolute(mcppls::platform::env::arguments().front()) };
    const std::string mock { base::join_path(base::parent_path(self), "mcppls-mock-mcpp") + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
    auto mockContent = fs::read_file(mock);
    if (!mockContent) {
        say("producer-candidate: {} is not built (needs mcppls-mock-mcpp beside mcppls-conformance)", mock);
        return 1;
    }
    const std::string binaryDirectory { base::join_path(home, ".xlings/data/xpkgs/xim-x-mcpp/9999.0.0/bin") };
    (void)fs::create_directories(binaryDirectory);
    const std::string binaryPath { base::join_path(binaryDirectory, "mcpp") + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
    if (auto written = fs::write_file(binaryPath, *mockContent); !written) {
        say("producer-candidate: {}", written.error().message);
        return 1;
    }
    if (auto marked = fs::make_executable(std::vector<std::string> { binaryPath }); !marked) {
        say("producer-candidate: {}", marked.error().message);
        return 1;
    }
    auto at = [&](std::string_view relative) { return native(base::join_path(root, relative)); };
    const std::string consumer { at("src/consumer.cppm") };
    const std::string main { at("src/main.cpp") };
    const std::string generated { at("target/.build-mcpp/deps/xpkg@1.0.0/out/xpkg_lua_stdlib.cppm") };
    auto translation_unit = [&](const std::string& source, std::string_view role, Json provides, Json requires_) {
        return Json { { "source", source }, { "work-directory", native(root) },
                      { "arguments", Json::array({ *clangxx, "-std=c++23", "-c", source, "-o", source + ".o" }) },
                      { "local-arguments", Json::array() }, { "object", source + ".o" },
                      { "provides", std::move(provides) }, { "requires", std::move(requires_) },
                      { "private", false }, { "ide", { { "role", role } } } };
    };
    const Json database {
        { "version", 1 }, { "revision", 0 },
        { "ide", { { "profile-version", "0.2.0" }, { "generator", { { "name", "mcpp" }, { "version", "9999.0.0" } } },
                   { "toolchains", { { "candidate", { { "family", "clang" }, { "version", "22" }, { "driver", *clangxx },
                                                       { "target", "x86_64-unknown-linux-gnu" } } } } } } },
        { "sets", Json::array({
            Json { { "name", "app" }, { "family-name", "app" },
                   { "ide", { { "toolchain", "candidate" }, { "configuration", "dev" }, { "kind", "executable" } } },
                   { "baseline-arguments", Json::array({ "-std=c++23" }) }, { "visible-sets", Json::array({ "xpkg" }) },
                   { "translation-units", Json::array({
                       translation_unit(consumer, "module-interface", Json { { "app.consumer", "" } }, Json::array({ "xpkg.lua_stdlib" })),
                       translation_unit(main, "non-module", Json::object(), Json::array({ "app.consumer", "xpkg.lua_stdlib" })) }) } },
            Json { { "name", "xpkg" }, { "family-name", "xpkg" },
                   { "ide", { { "toolchain", "candidate" }, { "configuration", "dev" }, { "kind", "library" } } },
                   { "baseline-arguments", Json::array({ "-std=c++23" }) }, { "visible-sets", Json::array({ "app" }) },
                   { "translation-units", Json::array({ translation_unit(generated, "module-interface", Json { { "xpkg.lua_stdlib", "" } }, Json::array()) }) } },
        }) },
    };
    const Json config { { "database", database }, { "watch", Json::array({ "mcpp.toml", "src/**/*.cppm", "src/**/*.cpp" }) } };
    if (auto written = fs::write_file(base::join_path(binaryDirectory, "mcpp-mock.json"), config.dump(2)); !written) {
        say("producer-candidate: {}", written.error().message);
        return 1;
    }
    return 0;
}

// real-project plan RP1.1/RP1.3: a straight import chain of `argument` modules (default 100),
// gen.chain0 through gen.chain<count-1>, whose base (gen.chain0) does not compile -- an undeclared
// name, the same shape as module-faults' broken.e, at the scale a real dependency's failure
// closure reaches (the plan's own measurement: totals grew from 21 to 101 preparing a single
// failed package). A plain importer of the chain's last module (never a unit of gen itself, the
// way xlings' main.cpp imported mcpplibs.xpkg.executor) and two modules entirely outside the chain
// prove the closure stays where it is at this scale: outside files keep answering, the importer is
// answered by mcppls's own engine at once, and nothing restarts clangd or keeps priming a module
// already known to be doomed.
int prepare_failure_at_base(const std::string& argument) {
    int count { 100 };
    if (!argument.empty()) {
        try {
            count = std::max(2, std::stoi(argument));
        } catch (...) {
            say("failure-at-base: {} is not a module count", argument);
            return 1;
        }
    }
    const std::string root { fs::current_directory() };
    (void)fs::create_directories(base::join_path(root, "src/gen"));
    (void)fs::create_directories(base::join_path(root, "src/healthy"));
    for (int i { 0 }; i < count; ++i) {
        const std::string body { i == 0
            ? std::format("// The base of a {}-module import chain (real-project plan RP1.1): fails to compile,\n"
                          "// the same shape as module-faults' broken.e, at the scale a real dependency's\n"
                          "// failure closure reaches.\n"
                          "export module gen.chain0;\n\n"
                          "export int chain0() {{ return undeclared_base_symbol; }}\n", count)
            : std::format("export module gen.chain{0};\nimport gen.chain{1};\n\nexport int chain{0}() {{ return chain{1}() + 1; }}\n",
                          i, i - 1) };
        if (auto written = fs::write_file(base::join_path(root, std::format("src/gen/chain{}.cppm", i)), body); !written) {
            say("failure-at-base: {}", written.error().message);
            return 1;
        }
    }
    const std::string importer { std::format(
        "// A plain importer of the chain's last module, never a unit of gen itself: the way xlings'\n"
        "// main.cpp imported mcpplibs.xpkg.executor in the incident this fixture is named for (real-project\n"
        "// plan RP1.1).\n"
        "import gen.chain{0};\n\n"
        "int use_chain() {{ return chain{0}(); }}\n", count - 1) };
    if (auto written = fs::write_file(base::join_path(root, "src/gen-importer.cpp"), importer); !written) {
        say("failure-at-base: {}", written.error().message);
        return 1;
    }
    static constexpr std::string_view HEALTHY_A {
        "// Outside the failed closure entirely (real-project plan RP1.1): answers normally throughout.\n"
        "export module healthy.a;\n\n"
        "export int healthyValue() { return 7; }\n" };
    static constexpr std::string_view HEALTHY_B {
        "import healthy.a;\n\n"
        "int healthyUser() { return healthyValue() * 2; }\n" };
    if (auto written = fs::write_file(base::join_path(root, "src/healthy/a.cppm"), std::string { HEALTHY_A }); !written) {
        say("failure-at-base: {}", written.error().message);
        return 1;
    }
    if (auto written = fs::write_file(base::join_path(root, "src/healthy/b.cpp"), std::string { HEALTHY_B }); !written) {
        say("failure-at-base: {}", written.error().message);
        return 1;
    }
    return 0;
}

// 0.0.3 plan B1: a clangd that cannot run on this machine -- what the official arm64 build is on
// a system whose libstdc++ is too old. mcppls-mock-mcpp, copied to stand-in/clangd with a config
// beside it saying `unavailable`, writes that text to its standard error and exits 1 however it is
// started: the loader's message and exit, on every host.
int prepare_clangd_cannot_load() {
    const std::string self { absolute(mcppls::platform::env::arguments().front()) };
    const std::string mock { base::join_path(base::parent_path(self), "mcppls-mock-mcpp") + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
    auto mockContent = fs::read_file(mock);
    if (!mockContent) {
        say("clangd-cannot-load: {} is not built (needs mcppls-mock-mcpp beside mcppls-conformance)", mock);
        return 1;
    }
    const std::string directory { base::join_path(fs::current_directory(), "stand-in") };
    (void)fs::create_directories(directory);
    const std::string clangd { base::join_path(directory, "clangd") + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
    if (auto written = fs::write_file(clangd, *mockContent); !written) {
        say("clangd-cannot-load: {}", written.error().message);
        return 1;
    }
    if (auto marked = fs::make_executable(std::vector<std::string> { clangd }); !marked) {
        say("clangd-cannot-load: {}", marked.error().message);
        return 1;
    }
    const Json config { { "unavailable",
        "clangd: /lib/aarch64-linux-gnu/libstdc++.so.6: version `GLIBCXX_3.4.30' not found (required by clangd)\n" } };
    if (auto written = fs::write_file(base::join_path(directory, "mcpp-mock.json"), config.dump(2) + "\n"); !written) {
        say("clangd-cannot-load: {}", written.error().message);
        return 1;
    }
    return 0;
}

int prepare(const std::string& kind, const std::string& argument) {
    if (kind == "s1-two-sets") return prepare_s1_two_sets(argument);
    if (kind == "payload-corrupt") return prepare_payload_corrupt(argument);
    if (kind == "producer-candidate") return prepare_producer_candidate(argument);
    if (kind == "failure-at-base") return prepare_failure_at_base(argument);
    if (kind == "compdb-clang-cl-std") return prepare_compdb_msvc_std(true);
    if (kind == "compdb-clangxx-msvc-std") return prepare_compdb_msvc_std(false);
    if (kind == "generated-module-old-mcpp") return prepare_generated_module_compdb(argument);
    if (kind == "clangd-cannot-load") return prepare_clangd_cannot_load();
    say("prepare: unknown fixture kind {} (s1-two-sets, payload-corrupt, producer-candidate, failure-at-base, compdb-clang-cl-std, compdb-clangxx-msvc-std, generated-module-old-mcpp, clangd-cannot-load)", kind);
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace mcpplibs;
    int status { 0 };
    cmdline::App app { "mcppls-conformance" };
    (void)app.version(std::string { base::VERSION });
    (void)app.description("Run a conformance fixture against a language server");

    cmdline::App runCommand { "run" };
    (void)runCommand.description("Run one fixture");
    (void)runCommand.option("server").takes_value().help("The mcppls executable");
    (void)runCommand.option("fixture").takes_value().help("Fixture directory with scenario.json");
    (void)runCommand.option("payload").takes_value().help("Payload directory");
    (void)runCommand.option("clangd").takes_value().help("clangd executable");
    (void)runCommand.option("kit").takes_value().help("Semantic kit directory");
    (void)runCommand.option("timeout").takes_value().help("Seconds each check may take (default 180)");
    (void)runCommand.option("msvc-env").takes_value().help("File of NAME=value lines: the developer environment for fixtures that build with MSVC");
    (void)runCommand.option("keep").help("Keep the scratch workspace");
    (void)runCommand.option("verbose").help("Print server logs and status notifications");
    (void)runCommand.option("workspace-dir").takes_value().help("Directory reused across runs: the fixture is copied and prepared there once");
    (void)runCommand.option("cache-dir").takes_value().help("The server's cache directory, e.g. shared by a cold and a warm run");
    (void)runCommand.option("measure").takes_value().help("File the checks' timings are written to, as JSON");
    (void)runCommand.option("expect-warm").help("module-cache-reused checks fail unless an earlier run left module files in --cache-dir");
    (void)runCommand.option("navigation-budget").takes_value().help("Seconds the first navigation may take from initialize; more fails the run");
    (void)runCommand.option("no-dynamic-watch").help("Do not advertise didChangeWatchedFiles.dynamicRegistration, exercising the polling fallback");
    (void)runCommand.option("plain-client").help("Alias for --client plain");
    (void)runCommand.option("client").takes_value().help("The capabilities a real editor sends: vscode, neovim, zed or plain (default: this runner's own, the full experimental.cxxModules block)");
    (void)runCommand.option("stress-seed").takes_value().help("Overrides every stress check's own \"seed\" (mcppls-devtools stress --seed)");
    (void)runCommand.action([&](const cmdline::ParsedArgs& args) {
        Options options;
        options.server = absolute(args.value("server").value_or(""));
        options.fixture = absolute(args.value("fixture").value_or(""));
        options.payload = args.value("payload") ? absolute(*args.value("payload")) : std::string {};
        options.clangd = args.value("clangd") ? absolute(*args.value("clangd")) : std::string {};
        options.kit = args.value("kit") ? absolute(*args.value("kit")) : std::string {};
        options.msvcEnvironment = args.value("msvc-env") ? absolute(*args.value("msvc-env")) : std::string {};
        if (auto timeout = args.value("timeout")) options.timeout = std::chrono::seconds { std::stoi(*timeout) };
        options.keep = args.is_flag_set("keep");
        options.verbose = args.is_flag_set("verbose");
        options.workspaceDirectory = args.value("workspace-dir") ? absolute(*args.value("workspace-dir")) : std::string {};
        options.cacheDirectory = args.value("cache-dir") ? absolute(*args.value("cache-dir")) : std::string {};
        options.measureFile = args.value("measure") ? absolute(*args.value("measure")) : std::string {};
        options.expectWarm = args.is_flag_set("expect-warm");
        options.noDynamicWatch = args.is_flag_set("no-dynamic-watch");
        options.plainClient = args.is_flag_set("plain-client");
        if (auto clientName = args.value("client")) {
            if (*clientName == "vscode") options.client = Options::ClientProfile::vscode;
            else if (*clientName == "neovim") options.client = Options::ClientProfile::neovim;
            else if (*clientName == "zed") options.client = Options::ClientProfile::zed;
            else if (*clientName == "plain") options.client = Options::ClientProfile::plain;
            else {
                say("run: --client takes vscode, neovim, zed or plain, not {}", *clientName);
                status = 2;
                return;
            }
        }
        if (auto stressSeed = args.value("stress-seed")) {
            try {
                options.stressSeed = static_cast<std::uint64_t>(std::stoull(*stressSeed));
            } catch (...) {
                say("run: --stress-seed takes an integer");
                status = 2;
                return;
            }
        }
        if (auto budget = args.value("navigation-budget")) {
            try {
                options.navigationBudget = std::stod(*budget);
            } catch (...) {
                say("run: --navigation-budget takes seconds");
                status = 2;
                return;
            }
        }
        if (options.server.empty() || options.fixture.empty()) {
            say("run: --server and --fixture are required");
            status = 2;
            return;
        }
        status = run(options);
    });
    (void)app.subcommand(std::move(runCommand));

    cmdline::App prepareCommand { "prepare" };
    (void)prepareCommand.description("Generate what a fixture needs, in the current directory (a fixture's own prepare step)");
    (void)prepareCommand.arg("kind").required();
    (void)prepareCommand.arg("argument");
    (void)prepareCommand.action([&](const cmdline::ParsedArgs& args) {
        status = prepare(args.value("kind").value_or(""), args.value("argument").value_or(""));
    });
    (void)app.subcommand(std::move(prepareCommand));

    cmdline::App versionCommand { "version" };
    (void)versionCommand.description("Print the version");
    (void)versionCommand.action([](const cmdline::ParsedArgs&) { say("mcppls-conformance {}", base::VERSION); });
    (void)app.subcommand(std::move(versionCommand));

    const int parsed { app.run(argc, argv) };
    return parsed != 0 ? parsed : status;
}
