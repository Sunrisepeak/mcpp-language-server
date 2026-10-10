// mcppls-conformance: drives a language server through a fixture's scenario
// and reports each check (conformance/README.md).
//
//   mcppls-conformance run --server <mcppls> --fixture <dir> [--payload DIR] [--clangd PATH] [--kit DIR]
//                            [--msvc-env FILE] [--timeout SECONDS] [--keep] [--verbose]
//                            [--workspace-dir DIR] [--cache-dir DIR] [--measure FILE] [--expect-warm]
//                            [--navigation-budget SECONDS] [--stage NAME]
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
import mcppls.project.scan;
import mcppls.orchestrator.tokens;
import mcppls.bundle.redact;
import mcppls.bundle.zip;

namespace base = mcppls::base;
namespace fs = mcppls::platform::fs;
namespace lsp = mcppls::lsp;
namespace tokens = mcppls::orchestrator::tokens;
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
    std::string timingEvidenceFile;   // bounded post-check report, only for the tiny timing fixture
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
    // A directory the scenario's `server-path-prepend` puts first on the server's PATH (a POSIX list), for the fixtures whose
    // build tool is a script of their own (xmake-needs-download's xmake). Empty for every fixture that predates it.
    std::string pathPrepend;
    // issue #23 fix plan F18: where `bundle` checks leave a copy of the bundle they checked, for CI to keep; empty: nowhere.
    std::string keepBundles;
    // 0.0.7 plan 6.4: a check with a "stage" runs only when `--stage` names it (a fixture's cold start, warm start, edits and
    // faults are separate runs sharing one workspace and, but for the faults, one cache); without `--stage` every check runs.
    std::string stage;
};

// Replaces HOME (POSIX) and USERPROFILE (Windows) in a spawn's environment, so
// `mcppls::platform::dirs::home_directory()` -- and so producer negotiation's search of
// `<home>/.xlings/data/xpkgs` and `<home>/.mcpp/registry/data/xpkgs` (real-project plan RP2.1) --
// sees only what the fixture itself put there. A no-op when `options.isolatedHome` is empty, which
// is every fixture that predates it.
void apply_isolated_home(std::vector<std::string>& environment, const Options& options) {
    if (options.isolatedHome.empty()) return;
    // XDG_CONFIG_HOME too: with it set, clangd and the server read their user configuration from there, not from <home>/.config.
    const auto isHomeVariable = [](const std::string& entry) {
        return entry.starts_with("HOME=") || entry.starts_with("USERPROFILE=") || entry.starts_with("HOMEDRIVE=") || entry.starts_with("HOMEPATH=")
               || entry.starts_with("XDG_CONFIG_HOME=");
    };
    std::erase_if(environment, isHomeVariable);
    environment.push_back("HOME=" + options.isolatedHome);
    environment.push_back("USERPROFILE=" + options.isolatedHome);
}

// Puts `options.pathPrepend` first on the PATH of a spawn's environment.
void apply_path_prepend(std::vector<std::string>& environment, const Options& options) {
    if (options.pathPrepend.empty()) return;
    for (auto& entry : environment) {
        if (entry.starts_with("PATH=")) {
            entry = "PATH=" + options.pathPrepend + ":" + entry.substr(5);
            return;
        }
    }
    environment.push_back("PATH=" + options.pathPrepend);
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

// mcppls-clangd's owned module cache (Linux) publishes every module file as
// <modules>/.owned-payload-v1/generation-<slot>-<n>/payload.pcm, so neither the name nor the directory says whose it
// is. The file does: its control block names the unit it was built from (ORIGINAL_FILE), the first absolute source
// path in it, and the unit's own `export module` declaration names the module.
bool owned_payload(std::string_view path) { return path.contains("/.owned-payload-v1/") && path.ends_with("/payload.pcm"); }

std::string owned_payload_source(const std::string& path) {
    static std::map<std::string, std::string> known;
    if (const auto found = known.find(path); found != known.end()) return found->second;
    static const std::regex SOURCE { R"((/[\w./+-]+\.(?:cppm|ixx|cxxm|mpp|cc|cpp|cxx|c\+\+m)))" };
    std::string source;
    std::ifstream in { path, std::ios::binary };
    std::string head(1 << 16, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    std::smatch match;
    if (std::regex_search(head, match, SOURCE)) source = match[1].str();
    known.emplace(path, source);
    return source;
}

// "name" or "name:partition", as the unit declares it; empty when it declares no module.
std::string declared_module(const std::string& source) {
    const auto text = fs::read_file(source);
    if (!text) return {};
    const auto declaration = mcppls::project::scan_source(*text).declaration;
    if (!declaration) return {};
    return declaration->partition.empty() ? declaration->module : declaration->module + ":" + declaration->partition;
}

// The engine's published module files for a module under a cache directory, with their stamps. clangd
// publishes <module>.pcm (a partition as <module>-<partition>.pcm) under a directory per source and
// command; the copies it hands to readers carry a timestamp in their names and are not included.
// mcppls-clangd's owned payloads are the module's when the unit they were built from declares it.
std::map<std::string, std::string> module_files(const std::string& cacheDirectory, std::string_view module) {
    std::string published { module };
    std::ranges::replace(published, ':', '-');
    const std::string declared { module };
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
            } else if (base::file_name(entry) == published || (owned_payload(entry) && declared_module(owned_payload_source(entry)) == declared)) {
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
    std::vector<std::string> serverCommand; // actual executable and argv, for timing evidence
    std::map<std::string, Json> diagnostics;     // uri -> latest diagnostics
    std::map<std::string, int> diagnosticsCount; // uri -> publishes received
    std::map<std::string, int> moduleFailedCount; // uri -> publishes received that carried a diagnostic with code "module-failed"
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
    // 0.0.7 plan 6.4 (scenario tests): every status with its progress and issue codes, so `timeline` can say how long a
    // project stayed not ready without a module being prepared; kept across server restarts, each marked in restartMarks.
    struct StatusSample {
        Clock::time_point at;
        std::string state;
        long done { 0 };
        long total { 0 };
        std::vector<std::string> issues;
    };
    std::vector<StatusSample> statusSamples;
    std::vector<Clock::time_point> restartMarks;
    std::map<std::string, Clock::time_point> diagnosticsAt;   // uri -> when its latest diagnostics arrived
    // Requests sent without waiting (typing keeps going while a completion is unanswered, as it does in an editor).
    struct AsyncRequest {
        std::string method;
        Clock::time_point sent;
        std::optional<Clock::time_point> answered;
        bool isError { false };
        Json result { nullptr };
    };
    std::map<std::int64_t, AsyncRequest> asyncRequests;
    // The server's OS pid where the platform can say; read by the clangd watcher's thread, so atomic.
    std::atomic<std::int64_t> serverPid { -1 };

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
        serverCommand = { spawn.program };
        serverCommand.insert(serverCommand.end(), spawn.arguments.begin(), spawn.arguments.end());
        spawn.workDirectory = workspace;
        auto environment = mcppls::platform::env::variables();
        environment.push_back("MCPPLS_CACHE_DIR=" + cacheDirectory);
        apply_isolated_home(environment, options);
        apply_path_prepend(environment, options);
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
        serverPid = connection_->native_pid().value_or(-1);
        return {};
    }

    // A new server after this one was killed or stopped (`fault` checks): what belongs to the old session goes, what the
    // scenario's timeline is made of stays.
    void reset() {
        connection_.reset();
        inbox_ = std::make_shared<mcppls::platform::Channel<Json>>();
        unanswered_ = 0;
        diagnostics.clear();
        diagnosticsCount.clear();
        moduleFailedCount.clear();
        diagnosticsAt.clear();
        status = Json {};
        statusByRoot.clear();
        watchers.clear();
        asyncRequests.clear();
        serverPid = -1;
        restartMarks.push_back(Clock::now());
        statusSamples.push_back({ Clock::now(), "restart", 0, 0, {} });
    }

    void notify(std::string_view method, Json params) { (void)connection_->send(lsp::make_notification(method, std::move(params))); }

    std::int64_t send_async(std::string_view method, Json params) {
        const std::int64_t id { nextId_++ };
        asyncRequests[id] = { std::string { method }, Clock::now(), std::nullopt, false, Json { nullptr } };
        (void)connection_->send(lsp::make_request(id, method, std::move(params)));
        return id;
    }

    // Reads what the server sends until `deadline`, without waiting for anything in particular.
    void pump_until(Clock::time_point deadline) {
        while (Clock::now() < deadline) {
            auto message = inbox_->pop_until(deadline);
            if (message) {
                dispatch(*message);
            } else if (inbox_->closed() && inbox_->size() == 0) {
                std::this_thread::sleep_for(std::min(std::chrono::milliseconds { 50 }, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())));
            }
        }
    }

    bool server_alive() { return connection_ != nullptr && !connection_->exit_code().has_value(); }

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

    // For `duration`, whatever arrives meanwhile: unlike drain, a server that keeps talking does not make it longer.
    void pump_for(std::chrono::milliseconds duration) {
        const auto until = Clock::now() + duration;
        while (auto message = inbox_->pop_until(until)) dispatch(*message);
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
                diagnosticsAt[uri] = Clock::now();
                ++diagnosticsCount[uri];
                if (std::ranges::any_of(diagnostics[uri], [](const Json& one) { const auto code = one.find("code"); return code != one.end() && code->is_string() && code->get<std::string>() == "module-failed"; })) {
                    ++moduleFailedCount[uri];
                }
                const std::string state { status.is_object() ? status.value("state", std::string {}) : std::string {} };
                if (!firstDiagnostics && (state == "ready" || state == "degraded")) firstDiagnostics = Clock::now();
            } else if (method == "cxxModules/status") {
                status = message["params"];
                const std::string statusRoot { status.value("project", Json::object()).value("root", std::string {}) };
                statusByRoot[statusRoot] = status;
                statusHistory.push_back(status.value("state", std::string {}));
                statusHistoryByRoot[statusRoot].push_back(statusHistory.back());
                statusTimeline.emplace_back(Clock::now(), statusHistory.back());
                {
                    StatusSample sample { Clock::now(), statusHistory.back(), 0, 0, {} };
                    if (const Json* progress { lsp::find(status, "progress") }; progress != nullptr && progress->is_object()) {
                        sample.done = progress->value("done", 0L);
                        sample.total = progress->value("total", 0L);
                    }
                    for (const auto& issue : status.value("issues", Json::array())) sample.issues.push_back(issue.value("code", std::string {}));
                    statusSamples.push_back(std::move(sample));
                }
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
        case lsp::Kind::response: {
            const Json* id { lsp::find(message, "id") };
            if (id == nullptr || !id->is_number_integer()) break;
            const auto it = asyncRequests.find(id->get<std::int64_t>());
            if (it == asyncRequests.end() || it->second.answered) break;
            it->second.answered = Clock::now();
            it->second.isError = message.contains("error");
            it->second.result = message.value("result", Json {});
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
        apply_path_prepend(environment, options);
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
// one of equals, contains, min-items, max-items, at-least and at-most (numbers), max-matches, min-matches, exists or absent, and holds when any value the pointer names satisfies it --
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
        } else if (expectation.contains("max-matches") || expectation.contains("min-matches")) {
            // How many of the values the pointer names satisfy "equals" (or, for strings, "contains"), at most / at least.
            const auto matching = std::ranges::count_if(matches, [&](const Json* match) {
                if (expectation.contains("equals")) return *match == expectation["equals"];
                if (expectation.contains("contains") && expectation["contains"].is_string()) {
                    return match->is_string() && match->get<std::string>().find(expectation["contains"].get<std::string>()) != std::string::npos;
                }
                return true;
            });
            held = matching <= expectation.value("max-matches", std::numeric_limits<long>::max()) && matching >= expectation.value("min-matches", 0L);
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
        } else if (expectation.contains("at-most")) {
            const double wanted { expectation.value("at-most", 0.0) };
            held = std::ranges::any_of(matches, [&](const Json* match) { return match->is_number() && match->get<double>() <= wanted; });
        } else if (expectation.contains("at-least")) {
            const double wanted { expectation.value("at-least", 0.0) };
            held = std::ranges::any_of(matches, [&](const Json* match) { return match->is_number() && match->get<double>() >= wanted; });
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

// M-3 (plan 0.0.9): clangd itself, started by the runner with the arguments a check names and no server between it and the
// check. A workaround's canary asks it what the defect is, and a latency budget is held against what it alone does. It
// speaks only what those need: documents, diagnostics and requests.
class DirectClangd {
private:
    std::unique_ptr<lsp::Connection> connection_;
    std::shared_ptr<mcppls::platform::Channel<Json>> inbox_ { std::make_shared<mcppls::platform::Channel<Json>>() };
    std::int64_t nextId_ { 1 };
    std::map<std::string, int> versions_;

    void dispatch(const Json& message) {
        switch (lsp::kind_of(message)) {
        case lsp::Kind::request: (void)connection_->send(lsp::make_result(message["id"], Json(nullptr))); break;
        case lsp::Kind::notification:
            if (message.value("method", std::string {}) == "textDocument/publishDiagnostics") {
                const std::string documentUri { message["params"].value("uri", std::string {}) };
                diagnostics[documentUri] = message["params"].value("diagnostics", Json::array());
                ++published[documentUri];
                lastPublished = Clock::now();
            }
            break;
        default: break;
        }
    }

public:
    std::map<std::string, Json> diagnostics;   // uri -> latest diagnostics
    std::map<std::string, int> published;      // uri -> publishes received
    Clock::time_point lastPublished {};

    ~DirectClangd() { stop(); }

    base::Result<void> start(const std::string& program, std::vector<std::string> arguments, const std::string& directory,
                             std::vector<std::string> environment, std::chrono::seconds timeout) {
        mcppls::platform::SpawnOptions spawn;
        spawn.program = program;
        spawn.arguments = std::move(arguments);
        spawn.workDirectory = directory;
        spawn.environment = std::move(environment);
        auto inbox = inbox_;
        auto connection = lsp::Connection::start(std::move(spawn), [inbox](Json message) { inbox->push(std::move(message)); }, [inbox] { inbox->close(); });
        if (!connection) return std::unexpected { connection.error() };
        connection_ = std::move(*connection);
        const auto initialized = request("initialize", Json { { "processId", nullptr }, { "rootUri", base::path_to_uri(directory) },
                                                              { "capabilities", Json { { "textDocument", Json { { "publishDiagnostics", Json::object() } } } } } }, timeout);
        if (!initialized) return base::fail("clangd-silent", "clangd did not answer initialize");
        (void)connection_->send(lsp::make_notification("initialized", Json::object()));
        return {};
    }

    void stop() {
        if (!connection_) return;
        connection_->stop(std::chrono::milliseconds { 2000 });
        connection_.reset();
    }

    void open(const std::string& path, const std::string& text) {
        versions_[path] = 1;
        (void)connection_->send(lsp::make_notification("textDocument/didOpen",
            Json { { "textDocument", Json { { "uri", base::path_to_uri(path) }, { "languageId", "cpp" }, { "version", 1 }, { "text", text } } } }));
    }

    void change(const std::string& path, const std::string& text) {
        (void)connection_->send(lsp::make_notification("textDocument/didChange",
            Json { { "textDocument", Json { { "uri", base::path_to_uri(path) }, { "version", ++versions_[path] } } },
                   { "contentChanges", Json::array({ Json { { "text", text } } }) } }));
    }

    // Reads what clangd sends until `deadline`.
    void pump_until(Clock::time_point deadline) {
        while (Clock::now() < deadline) {
            if (auto message = inbox_->pop_until(deadline)) dispatch(*message);
            else if (inbox_->closed() && inbox_->size() == 0) return;
        }
    }

    bool alive() const { return connection_ != nullptr && !inbox_->closed(); }

    // The request's result; nullptr for an error answer, nothing when none came in time.
    std::optional<Json> request(std::string_view method, Json params, std::chrono::seconds timeout) {
        const std::int64_t id { nextId_++ };
        (void)connection_->send(lsp::make_request(id, method, std::move(params)));
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            auto message = inbox_->pop_until(deadline);
            if (!message) {
                if (inbox_->closed() && inbox_->size() == 0) break;
                continue;
            }
            if (lsp::kind_of(*message) == lsp::Kind::response && (*message)["id"] == Json(id)) {
                if (message->contains("error")) return Json(nullptr);
                return message->value("result", Json {});
            }
            dispatch(*message);
        }
        return std::nullopt;
    }

    // The file's diagnostics once clangd has gone quiet: the first publication, then no other for `quiet`. A file's
    // clang-tidy diagnostics may arrive in a publication of their own, after the compiler's.
    bool settle(const std::string& path, std::chrono::seconds timeout, std::chrono::milliseconds quiet) {
        const std::string documentUri { base::path_to_uri(path) };
        const auto deadline = Clock::now() + timeout;
        while (published[documentUri] == 0 && Clock::now() < deadline && alive()) pump_until(std::min(deadline, Clock::now() + std::chrono::milliseconds { 200 }));
        if (published[documentUri] == 0) return false;
        while (Clock::now() < deadline && Clock::now() - lastPublished < quiet) pump_until(std::min(deadline, lastPublished + quiet));
        return true;
    }

    // The lines (0-based) of the file's diagnostics with `code`, sorted.
    std::vector<int> lines_with(const std::string& path, std::string_view code) {
        std::vector<int> lines;
        for (const auto& diagnostic : diagnostics[base::path_to_uri(path)]) {
            if (diagnostic.value("code", Json {}) != Json(std::string { code })) continue;
            const Json* start { lsp::find_path(diagnostic, { "range", "start" }) };
            lines.push_back(start == nullptr ? -1 : start->value("line", -1));
        }
        std::ranges::sort(lines);
        return lines;
    }
};

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
public:
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

    // The kernel's short name of a process (`clangd` for the payload's clangd), for finding it under the server.
    static std::string comm_of(std::int64_t pid) {
        auto comm = fs::read_file(std::format("/proc/{}/comm", pid));
        if (!comm) return {};
        return std::string { base::trim(*comm) };
    }

    // VmHWM, the process's own high-water mark of resident memory: a peak that falls between two samples is not lost.
    static std::optional<double> hwm_kb_of(std::int64_t pid) {
        auto status = fs::read_file(std::format("/proc/{}/status", pid));
        if (!status) return std::nullopt;
        for (auto line : base::split_lines(*status)) {
            if (!line.starts_with("VmHWM:")) continue;
            std::istringstream rest { std::string { line.substr(6) } };
            double kb { 0 };
            rest >> kb;
            return rest.fail() ? std::nullopt : std::optional<double> { kb };
        }
        return std::nullopt;
    }

    // clangd processes among the descendants of `serverPid`.
    static std::vector<std::int64_t> clangd_pids(std::int64_t serverPid) {
        std::vector<std::int64_t> found;
        for (const auto pid : tree_pids(serverPid)) {
            if (pid != serverPid && comm_of(pid).starts_with("clangd")) found.push_back(pid);
        }
        return found;
    }

private:
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

// ---- scenario tests (0.0.7 plan 6.4, conformance/README.md): helpers shared by latency, typing, edit-save, fault, timeline, bmi-reuse, resources ----

// The first field of /proc/loadavg, recorded beside every measurement: a run on a loaded machine explains itself.
Json load_average() {
    auto text = fs::read_file("/proc/loadavg");
    if (!text) return nullptr;
    std::istringstream stream { *text };
    double load { 0 };
    stream >> load;
    return stream.fail() ? Json(nullptr) : Json(load);
}

// A budget number under `key`, when the check's "budget" names it.
std::optional<double> budget_number(const Json& check, std::string_view key) {
    const auto budget = check.find("budget");
    if (budget == check.end() || !budget->is_object()) return std::nullopt;
    const auto found = budget->find(std::string { key });
    if (found == budget->end() || !found->is_number()) return std::nullopt;
    return found->get<double>();
}

// A budget enforced as a maximum: adds a sentence to `failures` when `measured` is over it. A measurement a platform could
// not make (nullopt) is not enforced, as everywhere else in this runner.
void enforce_max(const Json& check, std::string_view key, std::optional<double> measured, std::string_view what, std::string_view unit,
                 std::vector<std::string>& failures) {
    const auto limit = budget_number(check, key);
    if (!limit || !measured || *measured <= *limit) return;
    failures.push_back(std::format("{} {:.2f}{} is over the budget {:.2f}{}", what, *measured, unit, *limit, unit));
}

void enforce_min(const Json& check, std::string_view key, std::optional<double> measured, std::string_view what,
                 std::vector<std::string>& failures) {
    const auto limit = budget_number(check, key);
    if (!limit || !measured || *measured >= *limit) return;
    failures.push_back(std::format("{} {:.2f} is under the budget {:.2f}", what, *measured, *limit));
}

Json latency_stats(std::vector<double> seconds) {
    std::ranges::sort(seconds);
    return Json { { "n", seconds.size() }, { "p50", percentile(seconds, 0.5) }, { "p95", percentile(seconds, 0.95) },
                  { "max", seconds.empty() ? 0.0 : seconds.back() } };
}

std::string state_or_none(const Json& status) { return status.is_object() ? status.value("state", std::string {}) : std::string {}; }

// Every module file clangd published under a cache directory, path -> size and modification time. clangd keeps a copy for
// its readers with a timestamp in its name (`mcpp.log-20260930-010313-591880.pcm`): not a build, so not listed.
std::map<std::string, std::string> all_module_files(const std::string& cacheDirectory) {
    static const std::regex COPY { R"(-\d{8}-\d{6}-\d+\.pcm$)" };
    std::map<std::string, std::string> files;
    if (cacheDirectory.empty()) return files;
    std::vector<std::string> pending { cacheDirectory };
    while (!pending.empty()) {
        const std::string directory { pending.back() };
        pending.pop_back();
        for (const auto& entry : fs::list_directory(directory)) {
            if (fs::is_directory(entry)) {
                pending.push_back(entry);
                continue;
            }
            const std::string name { base::file_name(entry) };
            if (!name.ends_with(".pcm") || !entry.contains("/modules/") || std::regex_search(name, COPY)) continue;
            const auto stamp = fs::stamp(entry);
            files[entry] = stamp ? std::format("{}:{}", stamp->size, stamp->modified) : std::string {};
        }
    }
    return files;
}

// The module files the engine wrote since `before` (a file that was not there, or is there with another stamp), told
// apart: `rebuilt` for a unit that had a module file before -- what a warm start or a restart must not do -- and `first`
// for one that had none, a module no file of the earlier session needed (xlings: the files a warm session opens need 24
// modules the cold one never built).
struct Builds {
    std::size_t rebuilt { 0 };
    std::size_t first { 0 };
};
Builds builds_since(const std::map<std::string, std::string>& before, const std::map<std::string, std::string>& now) {
    // A module file is <modules>/<unit>-<hash>/<command hash>/<name>.pcm: its unit is two directories up. An owned
    // payload's unit is the source it was built from.
    const auto unit_of = [](const std::string& path) {
        return owned_payload(path) ? owned_payload_source(path) : base::parent_path(base::parent_path(path));
    };
    const auto size_of = [](const std::string& stamp) { return stamp.substr(0, stamp.find(':')); };
    std::set<std::string> unitsBefore;
    std::set<std::pair<std::string, std::string>> ownedBefore;   // (unit, size) of each owned payload there was
    for (const auto& [path, stamp] : before) {
        unitsBefore.insert(unit_of(path));
        if (owned_payload(path)) ownedBefore.emplace(unit_of(path), size_of(stamp));
    }
    Builds builds;
    for (const auto& [path, stamp] : now) {
        const auto it = before.find(path);
        if (it != before.end() && it->second == stamp) continue;
        // An owned read copy is a new generation holding the very bytes of a payload there was: not a build.
        if (it == before.end() && owned_payload(path) && ownedBefore.contains({ unit_of(path), size_of(stamp) })) continue;
        if (unitsBefore.contains(unit_of(path))) ++builds.rebuilt;
        else ++builds.first;
    }
    return builds;
}

// The server's own log files written since the run started, read as they grow and counted for the phrases a scenario test asks
// about ("Built module", "Still waiting for module lock"). The log is rotated at 5 MB with two files kept, and at the debug level
// clangd's own output fills that in a couple of minutes: a count made once at the end would have lost the start of the run, so the
// files are followed. A rotation is noticed by the file getting shorter, and what was unread of it is read from its `.1`.
class LogTailer {
private:
    static constexpr std::array<std::string_view, 2> NEEDLES { "Built module", "Still waiting for module lock" };
    std::string directory_;
    std::set<std::string> before_;
    std::mutex mutex_;
    std::mutex pollMutex_;
    std::map<std::string, std::uintmax_t> position_;
    std::map<std::string, std::uintmax_t> size_;
    std::map<std::string, long> counts_;
    bool clangdOutputSeen_ { false };
    std::jthread thread_;

    // Complete lines of `path` from `from` to `to`, counted; returns how far it got.
    std::uintmax_t read_lines(const std::string& path, std::uintmax_t from, std::uintmax_t to) {
        if (to <= from) return from;
        std::ifstream stream { std::filesystem::path { path }, std::ios::binary };
        if (!stream) return from;
        stream.seekg(static_cast<std::streamoff>(from));
        std::string chunk(static_cast<std::size_t>(to - from), '\0');
        stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        chunk.resize(static_cast<std::size_t>(stream.gcount()));
        const std::size_t end { chunk.rfind('\n') };
        if (end == std::string::npos) return from;
        std::lock_guard lock { mutex_ };
        std::size_t start { 0 };
        while (start <= end) {
            std::size_t stop { chunk.find('\n', start) };
            const std::string_view line { std::string_view { chunk }.substr(start, stop - start) };
            if (line.contains(" clangd (")) clangdOutputSeen_ = true;
            for (const auto needle : NEEDLES) {
                if (line.contains(needle)) ++counts_[std::string { needle }];
            }
            start = stop + 1;
        }
        return from + end + 1;
    }

public:
    void poll() {
        if (directory_.empty()) return;
        std::lock_guard polling { pollMutex_ };
        for (const auto& path : fs::list_directory(directory_)) {
            const std::string name { base::file_name(path) };
            if (!name.starts_with("server-") || !name.ends_with(".log") || before_.contains(path)) continue;
            std::error_code failed;
            const auto size { std::filesystem::file_size(path, failed) };
            if (failed) continue;
            const auto known { size_.find(path) };
            auto& position { position_[path] };
            if (known != size_.end() && size < known->second) {
                // Rotated: what was not read of the old content is in `<path>.1`.
                const std::string old { path + ".1" };
                std::error_code oldFailed;
                position = read_lines(old, position, std::filesystem::file_size(old, oldFailed));
                position = 0;
            }
            size_[path] = size;
            position = read_lines(path, position, size);
        }
    }

    void start(std::string directory, std::set<std::string> before) {
        directory_ = std::move(directory);
        before_ = std::move(before);
        thread_ = std::jthread { [this](std::stop_token token) {
            while (!token.stop_requested()) {
                poll();
                std::this_thread::sleep_for(std::chrono::milliseconds { 500 });
            }
        } };
    }

    long count(std::string_view needle) {
        std::lock_guard lock { mutex_ };
        const auto it = counts_.find(std::string { needle });
        return it == counts_.end() ? 0 : it->second;
    }

    // Whether the files carry clangd's own output: at the default log level they do not, and a count of nothing then says nothing.
    bool clangd_output_seen() {
        std::lock_guard lock { mutex_ };
        return clangdOutputSeen_;
    }

    ~LogTailer() {
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
    }
};

// SIGKILL where the platform names processes by number. False elsewhere: a fault the platform cannot cause is skipped, not passed.
bool kill_process(std::int64_t pid) {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        (void)pid;
        return false;
    } else {
        mcppls::platform::SpawnOptions spawn;
        spawn.program = "/bin/sh";
        spawn.arguments = { "-c", std::format("kill -KILL {}", pid) };
        auto result = mcppls::platform::run(std::move(spawn), std::chrono::seconds { 10 });
        return result && result->exitCode == 0;
    }
}

bool process_alive(std::int64_t pid) { return fs::exists(std::format("/proc/{}", pid)); }

std::string host_name() {
    if (auto text = fs::read_file("/proc/sys/kernel/hostname")) return std::string { base::trim(*text) };
    return mcppls::platform::env::get("HOSTNAME").value_or("localhost");
}

// 0.0.7 plan 6.4 (scenario tests): the clangd under the server, sampled from /proc every half second for the whole run.
// `ProcessSampler` above measures a window one check asks for; scenario tests ask afterwards ("how much of a core did
// clangd use while nobody was asking?") and for the peak over the whole run, across the restarts a fault causes. Where the
// platform is not Linux, or the server's pid is not known, nothing is sampled and every answer is nullopt: never a failure.
class ClangdWatcher {
private:
    struct Sample {
        Clock::time_point at;
        double cpuSeconds;   // across every clangd pid ever seen: a killed clangd's cost is not lost
    };
    std::function<std::int64_t()> serverPid_;
    std::mutex mutex_;
    std::map<std::int64_t, long> ticksByPid_;
    std::vector<Sample> samples_;
    double peakMB_ { 0 };
    bool active_ { false };
    std::jthread thread_;

    void sample() {
        const std::int64_t server { serverPid_() };
        if (server <= 0) return;
        const auto pids = ProcessSampler::clangd_pids(server);
        double rssKB { 0 };
        double hwmKB { 0 };
        std::lock_guard lock { mutex_ };
        for (const auto pid : pids) {
            if (auto ticks = ProcessSampler::cpu_ticks_of(pid)) {
                auto& best = ticksByPid_[pid];
                best = std::max(best, *ticks);
            }
            if (auto kb = ProcessSampler::rss_kb_of(pid)) rssKB += *kb;
            if (auto kb = ProcessSampler::hwm_kb_of(pid)) hwmKB = std::max(hwmKB, *kb);
        }
        peakMB_ = std::max({ peakMB_, rssKB / 1024.0, hwmKB / 1024.0 });
        long ticks { 0 };
        for (const auto& [pid, best] : ticksByPid_) ticks += best;
        samples_.push_back({ Clock::now(), static_cast<double>(ticks) / static_cast<double>(ProcessSampler::TICKS_PER_SECOND) });
    }

public:
    explicit ClangdWatcher(std::function<std::int64_t()> serverPid) : serverPid_ { std::move(serverPid) } {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::linux) {
            if (!fs::is_directory("/proc")) return;
            active_ = true;
            thread_ = std::jthread { [this](std::stop_token token) {
                while (!token.stop_requested()) {
                    sample();
                    std::this_thread::sleep_for(std::chrono::milliseconds { 500 });
                }
            } };
        }
    }

    bool active() const { return active_; }

    // The largest resident set clangd had, in megabytes, over the run so far; nullopt without /proc or before clangd ran.
    std::optional<double> peak_rss_mb() {
        std::lock_guard lock { mutex_ };
        return active_ && peakMB_ > 0 ? std::optional<double> { peakMB_ } : std::nullopt;
    }

    // clangd's resident set now, in megabytes.
    std::optional<double> rss_mb_now() {
        if (!active_ || serverPid_() <= 0) return std::nullopt;
        double kb { 0 };
        for (const auto pid : ProcessSampler::clangd_pids(serverPid_())) kb += ProcessSampler::rss_kb_of(pid).value_or(0.0);
        return kb > 0 ? std::optional<double> { kb / 1024.0 } : std::nullopt;
    }

    // Cores clangd used on average between two times (1.0 is one core busy throughout); nullopt when the window holds
    // fewer than two samples a few seconds apart.
    std::optional<double> cpu_cores(Clock::time_point from, Clock::time_point to) {
        std::lock_guard lock { mutex_ };
        const Sample* first { nullptr };
        const Sample* last { nullptr };
        for (const auto& s : samples_) {
            if (s.at < from || s.at > to) continue;
            if (first == nullptr) first = &s;
            last = &s;
        }
        if (first == nullptr || last == first) return std::nullopt;
        const double span { std::chrono::duration<double>(last->at - first->at).count() };
        if (span < 2.0) return std::nullopt;
        return (last->cpuSeconds - first->cpuSeconds) / span;
    }

    // clangd's own pids right now.
    std::vector<std::int64_t> pids() const {
        const std::int64_t server { serverPid_() };
        if (!active_ || server <= 0) return {};
        return ProcessSampler::clangd_pids(server);
    }

    ~ClangdWatcher() {
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
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
    Json semanticTokensLegend_ = Json::object();                // initialize's capabilities.semanticTokensProvider.legend
    Json capabilities_ = Json::object();                        // initialize's capabilities, for "capabilities" checks

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
    // What the run needs to know that the scenario's own constructor does not: when it began, the initialize a restarted server
    // is sent again, and what the cache held before the server started.
    void set_scenario_context(Clock::time_point began, Json initializeParams, std::map<std::string, std::string> pcmBefore, std::set<std::string> logsBefore) {
        begin_ = began;
        initializeParams_ = std::move(initializeParams);
        pcmBefore_ = std::move(pcmBefore);
        tailer_.start(base::join_path(cacheDirectory_, "logs"), std::move(logsBefore));
    }

    Json take_measure() { return std::exchange(measure_, Json(nullptr)); }

    // Seconds from initialize to now, for a check's "within-since-start".
    double since_start() const { return seconds_since(begin_); }

    // A check's "not-before-since-start": the client keeps being served until that many seconds have passed since initialize, so a
    // check can look at the server in a window (a producer that has not answered yet) and not only when something first holds.
    void wait_until_since_start(double seconds) {
        while (since_start() < seconds) client_.pump_until(Clock::now() + std::chrono::milliseconds { 100 });
    }

    void finish() {
        restore_files();
        if (mcp_) mcp_->stop();
        if (mcpDaemon_) mcpDaemon_->stop();
    }

    Scenario(Client& client, const Options& options, std::vector<std::string> serverArguments, std::string workspace, std::chrono::seconds timeout,
             std::map<std::string, std::string> prepared, std::string cacheDirectory, bool expectWarm,
             std::map<std::string, std::map<std::string, std::string>> moduleFilesBefore, Json semanticTokensLegend = Json::object(),
             Json capabilities = Json::object())
        : client_ { client }, options_ { options }, serverArguments_ { std::move(serverArguments) }, workspace_ { std::move(workspace) }, timeout_ { timeout }, prepared_ { std::move(prepared) },
          cacheDirectory_ { std::move(cacheDirectory) }, expectWarm_ { expectWarm }, moduleFilesBefore_ { std::move(moduleFilesBefore) },
          semanticTokensLegend_ ( std::move(semanticTokensLegend) ), capabilities_ ( std::move(capabilities) ) {}

    std::string uri(std::string_view relative) const { return base::path_to_uri(base::join_path(workspace_, relative)); }

    // A completion request at the check's "at"; "trigger" sends it as typing that character asked for it
    // (CompletionTriggerKind.TriggerCharacter), the way an editor does for a trigger character.
    Json completion_params(const Json& check, std::string_view file) const {
        Json params { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } };
        if (const auto trigger = check.find("trigger"); trigger != check.end() && trigger->is_string()) {
            params["context"] = Json { { "triggerKind", 2 }, { "triggerCharacter", trigger->get<std::string>() } };
        }
        return params;
    }

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

    // A `textDocument/semanticTokens/full` (or `/range`) result, decoded with the legend
    // `initialize` gave, and the text of the token it names -- from `content`, which must be the
    // buffer the request was answered against.
    struct DecodedToken {
        int line { 0 };
        int startChar { 0 };
        int length { 0 };
        std::string type;
        std::vector<std::string> modifiers;
        std::string text;
    };

    std::vector<DecodedToken> decode_semantic_tokens(const Json& result, const std::string& content) const {
        const Json types = semanticTokensLegend_.value("tokenTypes", Json::array());
        const Json modifiers = semanticTokensLegend_.value("tokenModifiers", Json::array());
        const auto lines = base::split_lines(content);
        std::vector<DecodedToken> decoded;
        for (const auto& token : tokens::decode(result)) {
            DecodedToken entry;
            entry.line = token.line;
            entry.startChar = token.startChar;
            entry.length = token.length;
            entry.type = token.type < types.size() && types[token.type].is_string() ? types[token.type].get<std::string>() : std::string {};
            for (std::size_t bit = 0; bit < modifiers.size(); ++bit) {
                if ((token.modifiers & (1u << bit)) != 0 && modifiers[bit].is_string()) entry.modifiers.push_back(modifiers[bit].get<std::string>());
            }
            if (token.line >= 0 && static_cast<std::size_t>(token.line) < lines.size()) {
                const std::string_view lineText { lines[static_cast<std::size_t>(token.line)] };
                if (token.startChar >= 0 && static_cast<std::size_t>(token.startChar) <= lineText.size()) {
                    const std::size_t available { lineText.size() - static_cast<std::size_t>(token.startChar) };
                    entry.text = std::string { lineText.substr(static_cast<std::size_t>(token.startChar), std::min<std::size_t>(available, static_cast<std::size_t>(std::max(0, token.length)))) };
                }
            }
            decoded.push_back(std::move(entry));
        }
        return decoded;
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

    // ---- scenario tests (0.0.7 plan 6.4; conformance/README.md "Scenario checks") ----------------------------------------

    Clock::time_point begin_ { Clock::now() };                    // initialize was sent: "since start" is measured from here
    Json initializeParams_ = Json::object();                      // sent again to a server a fault restarted
    std::map<std::string, std::string> pcmBefore_;                // module files in the cache before the server started
    LogTailer tailer_;                                            // counts what the server's log says as it is written
    std::map<std::string, std::string> restoreOnFinish_;          // absolute path -> what to write back when the run ends
    ClangdWatcher watcher_ { [this] { return client_.serverPid.load(); } };
    Json measure_ = nullptr;                                      // what the last check measured, for --measure

    static double seconds_since(Clock::time_point from) { return std::chrono::duration<double>(Clock::now() - from).count(); }
    static double seconds_between(Clock::time_point from, Clock::time_point to) { return std::chrono::duration<double>(to - from).count(); }

    // The first root's part of cxxModules/report, or null when the server did not answer in time.
    Json root_report(std::chrono::seconds timeout = std::chrono::seconds { 60 }) {
        auto answer = client_.request("cxxModules/report", Json::object(), timeout);
        if (!answer || !answer->is_object()) return nullptr;
        const Json roots = answer->value("roots", Json::array());
        return roots.is_array() && !roots.empty() ? roots.front() : Json(nullptr);
    }

    static long event_total(const Json& root, std::string_view kind) {
        if (!root.is_object() || !root.contains("eventTotals") || !root["eventTotals"].is_object()) return 0;
        return root["eventTotals"].value(std::string { kind }, 0L);
    }

    // How many requests of `method` each engine answered, as the report counts them (cumulative since the server started).
    static std::map<std::string, long> answered_by(const Json& root, std::string_view method) {
        std::map<std::string, long> counts;
        if (!root.is_object() || !root.contains("requests") || !root["requests"].is_object()) return counts;
        const auto it = root["requests"].find(std::string { method });
        if (it == root["requests"].end() || !it->contains("answeredBy")) return counts;
        for (const auto& item : (*it)["answeredBy"].items()) counts[item.key()] = item.value().get<long>();
        return counts;
    }

    // What the requests of `method` between two reports cost the core engine: the share of them some engine other than
    // mcppls's own fallback answered (clangd alone, or merged with mcppls's). nullopt when none were counted.
    static std::optional<double> engine_share(const Json& before, const Json& after, std::string_view method, Json* detail = nullptr) {
        const auto was = answered_by(before, method);
        const auto is = answered_by(after, method);
        long total { 0 };
        long fallback { 0 };
        Json counted = Json::object();
        for (const auto& [engine, count] : is) {
            const auto earlier = was.find(engine);
            const long delta { count - (earlier == was.end() ? 0 : earlier->second) };
            if (delta <= 0) continue;
            counted[engine] = delta;
            total += delta;
            if (engine == "mcppls") fallback += delta;
        }
        if (detail != nullptr) *detail = counted;
        if (total == 0) return std::nullopt;
        return static_cast<double>(total - fallback) / static_cast<double>(total);
    }

    // The status is ready or degraded and clangd has been quiet, under 0.3 of a core for eight seconds: what a person waits
    // for before typing into a project that was just opened. Without /proc, ready for five seconds is what is asked.
    bool settle(std::chrono::seconds timeout) {
        const auto deadline = Clock::now() + timeout;
        std::optional<Clock::time_point> readySince;
        while (Clock::now() < deadline) {
            client_.pump_until(std::min(deadline, Clock::now() + std::chrono::milliseconds { 500 }));
            const std::string state { state_of(client_.status) };
            if (state != "ready" && state != "degraded") {
                readySince.reset();
                continue;
            }
            if (!readySince) readySince = Clock::now();
            if (!watcher_.active()) {
                if (Clock::now() - *readySince >= std::chrono::seconds { 5 }) return true;
                continue;
            }
            const auto now = Clock::now();
            if (now - *readySince < std::chrono::seconds { 8 }) continue;
            const auto cores = watcher_.cpu_cores(now - std::chrono::seconds { 8 }, now);
            if (cores && *cores < 0.3) return true;
            // Ready for a minute is settled as a person sees it, whatever clangd does in the background meanwhile: R-5
            // builds implementation units for the index one at a time in idle time, a core busy for minutes on xlings.
            if (now - *readySince >= std::chrono::minutes { 1 }) return true;
        }
        return false;
    }

    // A file written back when the run ends, whatever became of the check that changed it: the workspace is reused by the
    // next stage, and a fixture's own sources must be the ones it was prepared with.
    void remember_original(const std::string& absolutePath) {
        if (restoreOnFinish_.contains(absolutePath)) return;
        restoreOnFinish_[absolutePath] = fs::read_file(absolutePath).value_or("");
    }

    void restore_files() {
        for (const auto& [path, content] : restoreOnFinish_) (void)fs::write_file(path, content);
        restoreOnFinish_.clear();
        for (const auto& path : createdOnFinish_) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        createdOnFinish_.clear();
    }

    struct ProbeRequest {
        std::string name;
        std::string kind;
        std::string method;
        std::string file;
        Json params;
        std::string expect;   // definition: a path suffix the answer must end with
    };

    std::optional<ProbeRequest> probe_request(const Json& one) const {
        ProbeRequest request;
        request.kind = one.value("kind", std::string {});
        request.file = one.value("file", std::string { "src/main.cpp" });
        request.name = one.value("name", request.kind);
        request.expect = one.value("expect", std::string {});
        Json params { { "textDocument", Json { { "uri", uri(request.file) } } } };
        if (request.kind == "completion" || request.kind == "hover" || request.kind == "definition") {
            request.method = request.kind == "completion" ? "textDocument/completion" : request.kind == "hover" ? "textDocument/hover" : "textDocument/definition";
            params["position"] = position(one.at("at"));
            if (request.kind == "completion" && one.contains("trigger")) {
                params["context"] = Json { { "triggerKind", 2 }, { "triggerCharacter", one.value("trigger", std::string {}) } };
            }
        } else if (request.kind == "semanticTokens") {
            request.method = "textDocument/semanticTokens/full";
        } else if (request.kind == "documentSymbol") {
            request.method = "textDocument/documentSymbol";
        } else {
            return std::nullopt;
        }
        request.params = std::move(params);
        return request;
    }

    static bool answer_empty(const ProbeRequest& request, const Json& result) {
        if (request.kind == "semanticTokens") return !result.is_object() || result.value("data", Json::array()).empty();
        return is_empty_result(request.method, result);
    }

    // Whether an answer to a request of `kind` is the right one: a `definition` names a location ending in `expected` (any
    // location when it is empty), a `completion` offers a label starting with it, a `hover` says it.
    static bool probe_right(const std::string& kind, const std::string& expected, const Json& result) {
        if (kind == "completion") {
            const auto labels = completion_labels(result);
            return std::ranges::any_of(labels, [&](const std::string& label) { return label.starts_with(expected); });
        }
        if (kind == "hover") return hover_text(result).contains(expected);
        const auto uris = location_uris(result);
        return expected.empty() ? !uris.empty() : std::ranges::any_of(uris, [&](const std::string& found) { return ends_with_path(found, expected); });
    }

    // How long from `since` until the probe is answered as it should be: nullopt when it never is within `wait`. A probe is a
    // `definition` (the default: a location ending in `expect`), a `completion` (a label starting with `expect`) or a `hover`
    // (text containing `expect`), at `at` in `file`. A jump is often answered by mcppls's own engine before clangd is up; a
    // completion of a std name is clangd's alone, so a fault that is about clangd asks for the latter.
    std::optional<double> time_to_answer(const Json& probe, Clock::time_point since, std::chrono::seconds wait) {
        const std::string file { probe.value("file", std::string { "src/main.cpp" }) };
        open(file);
        const std::string expected { probe.value("expect", std::string {}) };
        const std::string kind { probe.value("kind", std::string { "definition" }) };
        const std::string method { kind == "completion" ? "textDocument/completion" : kind == "hover" ? "textDocument/hover" : "textDocument/definition" };
        const auto deadline = Clock::now() + wait;
        while (Clock::now() < deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
            auto outcome = client_.request_full(method, Json { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(probe.at("at")) } },
                                                std::clamp(left, std::chrono::seconds { 1 }, std::chrono::seconds { 15 }));
            if (!outcome.timedOut && !outcome.isError && probe_right(kind, expected, outcome.result)) return seconds_since(since);
            client_.pump_until(Clock::now() + std::chrono::milliseconds { 300 });
        }
        return std::nullopt;
    }

    // A server that replaces the one a fault ended: the same arguments, workspace and cache, the same initialize, and every
    // document open that an editor still has open. False, with why, when it does not come up.
    bool restart_server(std::string& why) {
        client_.reset();
        if (auto started = client_.start(options_, serverArguments_, workspace_, cacheDirectory_); !started) {
            why = "cannot start the server again: " + started.error().message;
            return false;
        }
        auto initialized = client_.request("initialize", initializeParams_, std::chrono::seconds { 120 });
        if (!initialized || !initialized->is_object()) {
            why = "the new server did not answer initialize";
            return false;
        }
        client_.notify("initialized", Json::object());
        auto previous = std::move(open_);
        open_.clear();
        for (const auto& [path, buffer] : previous) open(path, buffer.first);
        return true;
    }

    // Whether the `ready` at `index` of the status history held: 0.0.6 says `ready` for a moment before it says `preparing` on a
    // start with nothing built, and that is not a project a person can use, so a ready that lasts less than a second and a half
    // (until the next state that is not ready, or until now) does not count.
    bool ready_holds(std::size_t index) const {
        const auto& samples = client_.statusSamples;
        if (index >= samples.size() || samples[index].state != "ready") return false;
        auto until { Clock::now() };
        for (std::size_t next { index + 1 }; next < samples.size(); ++next) {
            if (samples[next].state != "ready") {
                until = samples[next].at;
                break;
            }
        }
        return until - samples[index].at >= std::chrono::milliseconds { 1500 };
    }

    // Seconds from `since` until the status was ready again and stayed so: 0 when it never left ready; nullopt while it has not come back.
    std::optional<double> ready_seconds_since(Clock::time_point since) const {
        bool leftReady { false };
        for (std::size_t i { 0 }; i < client_.statusSamples.size(); ++i) {
            const auto& sample = client_.statusSamples[i];
            if (sample.at < since) continue;
            if (ready_holds(i)) return seconds_between(since, sample.at);
            if (sample.state != "ready") leftReady = true;
        }
        if (!leftReady && state_of(client_.status) == "ready") return 0.0;
        return std::nullopt;
    }

    bool preparing_since(Clock::time_point since) const {
        return std::ranges::any_of(client_.statusSamples, [&](const auto& sample) { return sample.at >= since && sample.state == "preparing"; });
    }

    // Waits until the preparation has got `fraction` of the way through, while it is still preparing. A status carries `progress`
    // only while the server counts what there is to prepare (xlings' start, whose transitional model is replaced, reports none):
    // then `fallback` seconds in the state `preparing` stand for it.
    bool wait_for_progress(double fraction, std::chrono::seconds timeout, std::chrono::seconds fallback = std::chrono::seconds { 3 }) {
        return client_.wait_for([&] {
            if (client_.statusSamples.empty()) return false;
            const auto& latest = client_.statusSamples.back();
            if (latest.state != "preparing") return false;
            if (latest.total > 0) return static_cast<double>(latest.done) >= fraction * static_cast<double>(latest.total);
            return Clock::now() - latest.at >= fallback;
        }, timeout);
    }

    std::pair<bool, std::string> finish_measure(std::vector<std::string> failures, Json summary, const std::string& brief) {
        summary["load"] = load_average();
        measure_ = summary;
        if (failures.empty()) return { true, brief };
        return { false, std::format("{}; {}", base::join(failures, "; "), brief) };
    }

    // ---- latency ----

    std::pair<bool, std::string> run_latency(const Json& check) {
        std::vector<ProbeRequest> specs;
        for (const auto& one : check.value("requests", Json::array())) {
            auto spec = probe_request(one);
            if (!spec) return { false, std::format("unknown request {}", lsp::dump(one)) };
            specs.push_back(std::move(*spec));
        }
        // "first-answers": requests asked in every round until each is answered as it should be; the time since initialize of the
        // first right answer is measured and held to its own "within" (the plan's U1: the first clangd completion in a heavy file,
        // the first correct jump), while the phase's other requests are timed as usual.
        struct FirstAnswer {
            ProbeRequest spec;
            std::string name;
            double within { 0 };
            std::optional<double> at;
        };
        std::vector<FirstAnswer> firsts;
        for (const auto& one : check.value("first-answers", Json::array())) {
            auto spec = probe_request(one);
            if (!spec) return { false, std::format("unknown first answer {}", lsp::dump(one)) };
            firsts.push_back({ std::move(*spec), one.value("name", std::string {}), one.value("within", 0.0), std::nullopt });
        }
        if (specs.empty() && firsts.empty()) return { false, "a latency check names no requests" };
        for (const auto& spec : specs) open(spec.file);
        for (const auto& first : firsts) open(first.spec.file);
        const std::string phase { check.value("phase", std::string { "now" }) };
        const std::chrono::milliseconds interval { check.value("interval-ms", 700) };
        const std::chrono::seconds requestTimeout { check.value("requestTimeout", 65) };
        const std::chrono::seconds startWithin { check.value("start-within", 240) };
        const std::chrono::seconds phaseSeconds { check.value("seconds", phase == "background" ? 45 : 600) };
        const int wantedRounds { check.value("rounds", phase == "idle" || phase == "now" ? 20 : 0) };
        const auto ready = [&] {
            const std::string state { state_of(client_.status) };
            return state == "ready" || state == "degraded";
        };
        if (phase == "cold") {
            // While the preparation runs: a check that starts once it is over would measure nothing of what it is named for.
            if (!client_.wait_for([&] { return state_of(client_.status) == "preparing"; }, startWithin)) {
                return { false, "the project was never preparing: a cold phase needs a cold cache" };
            }
        } else if (phase == "background") {
            if (!client_.wait_for(ready, startWithin)) return { false, "the project did not become ready" };
        } else if (phase == "idle") {
            if (!settle(startWithin)) return { false, "the project did not settle: clangd never went quiet" };
        } else if (phase != "now") {
            return { false, std::format("unknown phase {}", phase) };
        }

        const Json before = root_report();
        const auto began { Clock::now() };
        struct Counts {
            int timeouts { 0 };
            int errors { 0 };
            int empty { 0 };
            int wrong { 0 };
        };
        std::map<std::string, std::vector<double>> latencies;
        std::map<std::string, std::vector<double>> byName;   // the same, one entry per named request: where the slow one is
        std::map<std::string, Counts> counts;
        int rounds { 0 };
        std::string unusable;
        std::optional<Clock::time_point> graceEnd;
        const std::chrono::seconds grace { check.value("first-answers-grace", 60) };
        for (;;) {
            for (auto& first : firsts) {
                if (first.at) continue;
                const auto outcome { client_.request_full(first.spec.method, first.spec.params, requestTimeout) };
                if (!outcome.timedOut && !outcome.isError && probe_right(first.spec.kind, first.spec.expect, outcome.result)) first.at = since_start();
            }
            for (const auto& spec : specs) {
                const auto started { Clock::now() };
                const auto outcome { client_.request_full(spec.method, spec.params, requestTimeout) };
                latencies[spec.kind].push_back(seconds_since(started));
                byName[spec.name + " " + spec.file].push_back(latencies[spec.kind].back());
                auto& count = counts[spec.kind];
                if (outcome.timedOut) {
                    ++count.timeouts;
                } else if (outcome.isError) {
                    ++count.errors;
                } else {
                    if (answer_empty(spec, outcome.result)) ++count.empty;
                    if (!spec.expect.empty() && !probe_right(spec.kind, spec.expect, outcome.result)) ++count.wrong;
                }
            }
            ++rounds;
            unusable = client_.unusable();
            if (!unusable.empty()) break;
            const bool phaseOver { wantedRounds > 0 ? rounds >= wantedRounds
                                   : phase == "cold" ? state_of(client_.status) != "preparing" || Clock::now() - began >= phaseSeconds
                                                     : Clock::now() - began >= phaseSeconds };
            if (phaseOver) {
                // The first answers still owed get a grace period after the phase is over, and no more.
                if (std::ranges::all_of(firsts, [](const FirstAnswer& first) { return first.at.has_value(); })) break;
                if (!graceEnd) graceEnd = Clock::now() + grace;
                if (Clock::now() >= *graceEnd) break;
            }
            client_.pump_until(Clock::now() + interval);
        }
        const Json after = root_report();

        std::vector<std::string> failures;
        if (!unusable.empty()) failures.push_back(unusable);
        Json kinds = Json::object();
        std::vector<std::string> brief;
        for (const auto& [kind, samples] : latencies) {
            Json stats = latency_stats(samples);
            const auto& count = counts[kind];
            stats["timeouts"] = count.timeouts;
            stats["errors"] = count.errors;
            stats["empty"] = count.empty;
            stats["wrong"] = count.wrong;
            const auto limit = [&](std::string_view key) { return budget_number(check, std::format("{}.{}", kind, key)).or_else([&] { return budget_number(check, key); }); };
            const auto enforce = [&](std::string_view key, double measured, std::string_view unit) {
                if (const auto max = limit(key); max && measured > *max) failures.push_back(std::format("{} {} {:.2f}{} is over the budget {:.2f}{}", kind, key, measured, unit, *max, unit));
            };
            enforce("p50", stats["p50"].get<double>(), "s");
            enforce("p95", stats["p95"].get<double>(), "s");
            enforce("max", stats["max"].get<double>(), "s");
            if (const auto max = budget_number(check, "timeouts"); max && count.timeouts > *max) failures.push_back(std::format("{} {} timeout(s)", kind, count.timeouts));
            if (const auto max = budget_number(check, "wrong"); max && count.wrong > *max) failures.push_back(std::format("{} {} answer(s) not naming the expected file", kind, count.wrong));
            if (const auto max = budget_number(check, "maxEmptyShare"); max && !samples.empty() && static_cast<double>(count.empty) / static_cast<double>(samples.size()) > *max) {
                failures.push_back(std::format("{} {} of {} answers empty", kind, count.empty, samples.size()));
            }
            kinds[kind] = std::move(stats);
            brief.push_back(std::format("{} p95 {:.2f}s max {:.2f}s", kind, kinds[kind]["p95"].get<double>(), kinds[kind]["max"].get<double>()));
        }
        // Speed alone is not enough: a fallback answers at once. The share of answers a real engine gave is held too.
        Json shares = Json::object();
        std::set<std::string> shareKinds;
        for (const auto& kind : check.value("engine-share-kinds", Json::array({ "completion", "hover" }))) shareKinds.insert(kind.get<std::string>());
        for (const auto& spec : specs) {
            if (!shareKinds.contains(spec.kind) || shares.contains(spec.method)) continue;
            Json detail;
            const auto share = engine_share(before, after, spec.method, &detail);
            shares[spec.method] = Json { { "share", share ? Json(*share) : Json(nullptr) }, { "answeredBy", detail } };
            enforce_min(check, "engineShare", share, std::format("engine share of {}", spec.kind), failures);
            if (share) brief.push_back(std::format("{} by engine {:.0f}%", spec.kind, *share * 100.0));
        }
        Json firstAnswers = Json::object();
        for (const auto& first : firsts) {
            firstAnswers[first.name] = first.at ? Json(*first.at) : Json(nullptr);
            if (!first.at) failures.push_back(std::format("first answer '{}' never came", first.name));
            else if (first.within > 0 && *first.at > first.within) failures.push_back(std::format("first answer '{}' came {:.1f}s after initialize, over the budget {:.1f}s", first.name, *first.at, first.within));
            brief.push_back(std::format("{} first answered {}", first.name, first.at ? std::format("{:.1f}s in", *first.at) : std::string { "never" }));
        }
        Json requests = Json::object();
        for (const auto& [name, samples] : byName) requests[name] = latency_stats(samples);
        Json summary { { "phase", phase }, { "rounds", rounds }, { "seconds", seconds_since(began) }, { "kinds", std::move(kinds) }, { "requests", std::move(requests) },
                       { "firstAnswers", std::move(firstAnswers) }, { "engineShare", std::move(shares) } };
        return finish_measure(std::move(failures), std::move(summary), std::format("{}, {} round(s): {}", phase, rounds, base::join(brief, "; ")));
    }

    // ---- clangd on its own (M-3, M-2 of plan 0.0.9) ----

    // The clangd of the run: --clangd, else the payload's.
    std::string clangd_program() const {
        return !options_.clangd.empty() ? options_.clangd : base::join_path(options_.payload, "clangd/bin/clangd") + std::string { mcppls::os::EXECUTABLE_SUFFIX };
    }

    // A check's "retired-by": the capability that retires the workaround whose defect the check watches, for an engine
    // whose verified identity declares it (the server's own rule, engine/clangd.cpp). Such an engine does not have the
    // defect by proof, so there is nothing to watch; it still runs against any other clangd (--clangd, a payload without it).
    std::optional<std::string> retired_canary(const Json& check) const {
        const std::string feature { check.value("retired-by", std::string {}) };
        if (feature.empty() || !options_.clangd.empty() || options_.payload.empty()) return std::nullopt;
        const auto text = fs::read_file(base::join_path(options_.payload, "payload.json"));
        if (!text) return std::nullopt;
        const Json manifest = Json::parse(*text, nullptr, false);
        if (manifest.is_discarded() || manifest.value("payload-version", 0) < 4) return std::nullopt;
        const Json* identity { nullptr };
        if (const auto engines = manifest.find("engines"); engines != manifest.end() && engines->is_object()) {
            if (const auto clangd = engines->find("clangd"); clangd != engines->end() && clangd->is_object()) {
                if (const auto found = clangd->find("identity"); found != clangd->end() && found->is_object()) identity = &*found;
            }
        }
        if (identity == nullptr) return std::nullopt;
        for (const auto& declared : identity->value("features", Json::array())) {
            if (declared.is_string() && declared.get<std::string>() == feature) {
                return std::format("retired: the payload's engine {} declares {}", identity->value("engine-version", std::string {}), feature);
            }
        }
        return std::nullopt;
    }

    // A check's clangd arguments, `{workspace}` and `{engine-database}` (the directory of the compile_commands.json the server
    // wrote for its own clangd, so a baseline runs the very commands the server's does) replaced.
    std::vector<std::string> direct_arguments(const Json& list) {
        std::string database;
        std::vector<std::string> arguments;
        for (const auto& item : list) {
            std::string argument { base::replace_all(item.get<std::string>(), "{workspace}", workspace_) };
            if (argument.contains("{engine-database}")) {
                if (database.empty()) {
                    const Json root = root_report();
                    if (root.is_object()) database = base::join_path(root.value("cacheDirectory", std::string {}), "contexts/default/cdb");
                }
                argument = base::replace_all(argument, "{engine-database}", database);
            }
            arguments.push_back(std::move(argument));
        }
        return arguments;
    }

    std::vector<std::string> direct_environment() const {
        auto environment = mcppls::platform::env::variables();
        apply_isolated_home(environment, options_);
        return environment;
    }

    // One session of clangd alone on `file`, started with `arguments`: its completions at `at` timed in milliseconds, after
    // `warmup` that are not. With "edit", each is asked right after the buffer changed (a comment appended), as typing does.
    // Nothing when clangd could not be started or never published the file's diagnostics, with the reason in `why`.
    std::optional<std::vector<double>> direct_completions(const Json& check, const std::string& file, const Json& arguments, int rounds, int warmup,
                                                          std::string& why) {
        const std::string path { base::join_path(workspace_, file) };
        const std::string original { text_of(file) };
        DirectClangd clangd;
        if (auto started = clangd.start(clangd_program(), direct_arguments(arguments), workspace_, direct_environment(), std::chrono::seconds { 30 }); !started) {
            why = started.error().message;
            return std::nullopt;
        }
        clangd.open(path, original);
        if (!clangd.settle(path, timeout_, std::chrono::milliseconds { 0 })) {
            why = "clangd never published the diagnostics of " + file;
            return std::nullopt;
        }
        const bool edit { check.value("edit", false) };
        const std::chrono::milliseconds interval { check.value("interval-ms", 0) };
        const Json params { { "textDocument", Json { { "uri", base::path_to_uri(path) } } }, { "position", position(check.at("at")) } };
        std::vector<double> milliseconds;
        for (int round { 0 }; round < rounds + warmup; ++round) {
            if (edit) clangd.change(path, original + std::format("// edit {}\n", round));
            const auto started { Clock::now() };
            const auto answer = clangd.request("textDocument/completion", params, std::chrono::seconds { 60 });
            if (!answer) {
                why = "clangd did not answer a completion in 60 s";
                return std::nullopt;
            }
            if (round >= warmup) milliseconds.push_back(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
            if (interval.count() > 0) clangd.pump_until(Clock::now() + interval);
            client_.drain(std::chrono::milliseconds { 0 });
        }
        return milliseconds;
    }

    static double median_of(std::vector<double> samples) {
        std::ranges::sort(samples);
        return percentile(std::move(samples), 0.5);
    }

    // "clangd-lsp": the runner's clangd driven over LSP with the arguments the check names, the server not involved.
    //   "diagnostics": `file` is opened and, once clangd's diagnostics settle, `code` is on exactly the 0-based `lines`
    //     (none for an empty list), or, with "includes", on at least those.
    //   "completion-ratio": the median completion at `at` with "arguments" over the median with "baseline-arguments", over
    //     "passes" (default 2) alternating sessions of "rounds" (default 10) each; the check holds when the ratio is over
    //     "min-ratio".
    // A canary holds while the defect its workaround exists for is there; `says` is what a failure means, as in clangd-check.
    std::pair<bool, std::string> run_clangd_lsp(const Json& check, const std::string& file) {
        if (!fs::is_regular_file(clangd_program())) return { false, "no clangd: pass --clangd or --payload" };
        const std::string says { check.value("says", std::string {}) };
        const std::string action { check.value("action", std::string {}) };
        const auto failed = [&](const std::string& detail) {
            return std::pair<bool, std::string> { false, says.empty() ? detail : std::format("{} ({})", says, detail) };
        };
        const Json arguments = check.value("arguments", Json::array());
        if (action == "diagnostics") {
            const std::string path { base::join_path(workspace_, file) };
            DirectClangd clangd;
            if (auto started = clangd.start(clangd_program(), direct_arguments(arguments), workspace_, direct_environment(), std::chrono::seconds { 30 }); !started) {
                return { false, started.error().message };
            }
            clangd.open(path, text_of(file));
            const std::chrono::seconds within { check.value("seconds", 60) };
            if (!clangd.settle(path, within, std::chrono::milliseconds { check.value("quiet-ms", 3000) })) {
                return { false, std::format("clangd published no diagnostics of {} in {} s", file, within.count()) };
            }
            const std::string code { check.value("code", std::string {}) };
            const std::vector<int> lines { clangd.lines_with(path, code) };
            std::vector<int> wanted { check.value("lines", std::vector<int> {}) };
            std::ranges::sort(wanted);
            const bool held { check.value("includes", false) ? std::ranges::includes(lines, wanted) : lines == wanted };
            const std::string detail { std::format("{} on lines {}", code, lsp::dump(lines)) };
            return held ? std::pair<bool, std::string> { true, detail + ", as expected" } : failed(detail);
        }
        if (action == "completion-ratio") {
            const int rounds { check.value("rounds", 10) };
            const int passes { std::max(1, check.value("passes", 2)) };
            const double minimum { check.value("min-ratio", 1.8) };
            std::vector<double> with, without;
            std::string why;
            for (int pass { 0 }; pass < passes; ++pass) {
                for (const bool baseline : { false, true }) {
                    auto samples = direct_completions(check, file, check.value(baseline ? "baseline-arguments" : "arguments", Json::array()), rounds, 2, why);
                    if (!samples) return { false, why };
                    auto& into = baseline ? without : with;
                    into.insert(into.end(), samples->begin(), samples->end());
                }
            }
            const double ratio { median_of(without) > 0 ? median_of(with) / median_of(without) : 0.0 };
            measure_ = Json { { "with", latency_stats(with) }, { "without", latency_stats(without) }, { "ratio", ratio } };
            const std::string detail { std::format("completion median {:.0f} ms with the arguments, {:.0f} ms without: {:.2f}x", median_of(with), median_of(without), ratio) };
            return ratio > minimum ? std::pair<bool, std::string> { true, detail + std::format(", over {:.2f} as expected", minimum) } : failed(detail);
        }
        return { false, std::format("unknown clangd-lsp action '{}'", action) };
    }

    // "completion-baseline" (0.0.9 plan M-2): completion through the server held to what clangd alone does on the same file.
    // The server is let to settle first; then clangd alone (with "baseline-arguments", of which `{engine-database}` is the
    // server's own compile_commands.json) is timed over "rounds" completions at `at`, each right after an edit, and then
    // the server over the same. Budget: "ratio" and "slack-ms": the server's p95 is at most ratio x clangd's p95 + slack;
    // "engineShare" is the share of the server's answers that clangd gave, since an answer without it is fast and no use.
    std::pair<bool, std::string> run_completion_baseline(const Json& check, const std::string& file) {
        if (!fs::is_regular_file(clangd_program())) return { false, "no clangd: pass --clangd or --payload" };
        const int rounds { check.value("rounds", 40) };
        const std::chrono::seconds startWithin { check.value("start-within", 240) };
        open(file);
        if (!settle(startWithin)) return { false, "the project did not settle: clangd never went quiet" };
        // The server's first completion of a file waits for the file's preamble: not what is measured.
        const Json params { { "textDocument", Json { { "uri", uri(file) } } }, { "position", position(check.at("at")) } };
        const std::string original { text_of(file) };
        const std::chrono::milliseconds interval { check.value("interval-ms", 0) };
        const int warmup { 3 };
        std::string why;
        auto baseline = direct_completions(check, file, check.value("baseline-arguments", Json::array()), rounds, warmup, why);
        if (!baseline) return { false, why };
        const Json before = root_report();
        std::vector<double> served;
        int empty { 0 };
        for (int round { 0 }; round < rounds + warmup; ++round) {
            change(file, original + std::format("// edit {}\n", round));
            const auto started { Clock::now() };
            const auto outcome { client_.request_full("textDocument/completion", params, std::chrono::seconds { 60 }) };
            if (outcome.timedOut) return { false, "the server did not answer a completion in 60 s" };
            if (round < warmup) continue;
            served.push_back(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
            if (completion_labels(outcome.result).empty()) ++empty;
            client_.pump_until(Clock::now() + interval);
        }
        change(file, original);
        const Json after = root_report();
        const double ratio { budget_number(check, "ratio").value_or(1.3) };
        const double slack { budget_number(check, "slackMs").value_or(30.0) };
        std::ranges::sort(*baseline);
        std::ranges::sort(served);
        const double clangdP95 { percentile(*baseline, 0.95) };
        const double serverP95 { percentile(served, 0.95) };
        const double limit { ratio * clangdP95 + slack };
        std::vector<std::string> failures;
        if (serverP95 > limit) failures.push_back(std::format("completion p95 {:.0f} ms through the server is over {:.0f} ms (clangd alone {:.0f} ms x {:.2f} + {:.0f})", serverP95, limit, clangdP95, ratio, slack));
        if (empty > 0 && budget_number(check, "maxEmpty") && empty > *budget_number(check, "maxEmpty")) failures.push_back(std::format("{} of {} completions answered empty", empty, served.size()));
        Json detail;
        const auto share = engine_share(before, after, "textDocument/completion", &detail);
        enforce_min(check, "engineShare", share, "engine share of completion", failures);
        const Json requests { { "clangd", Json { { "p50Ms", percentile(*baseline, 0.5) }, { "p95Ms", clangdP95 }, { "maxMs", baseline->back() } } },
                              { "server", Json { { "p50Ms", percentile(served, 0.5) }, { "p95Ms", serverP95 }, { "maxMs", served.back() } } },
                              { "limitMs", limit }, { "engineShare", share ? Json(*share) : Json(nullptr) }, { "answeredBy", detail } };
        // What the report says of where the server's time went (M-1): the engine's share and the server's own.
        if (after.is_object()) {
            if (const Json* stats { lsp::find_path(after, { "requests", "textDocument/completion" }) }; stats != nullptr) measure_ = Json { { "comparison", requests }, { "report", *stats } };
        }
        if (!measure_.is_object()) measure_ = Json { { "comparison", requests } };
        measure_["load"] = load_average();
        const std::string brief { std::format("completion p50/p95 {:.0f}/{:.0f} ms through the server, {:.0f}/{:.0f} ms clangd alone, limit {:.0f} ms{}", percentile(served, 0.5), serverP95,
                                              percentile(*baseline, 0.5), clangdP95, limit, share ? std::format(", {:.0f}% by clangd", *share * 100.0) : std::string {}) };
        if (failures.empty()) return { true, brief };
        return { false, std::format("{}; {}", base::join(failures, "; "), brief) };
    }

    // ---- typing ----

    // Whether a diagnostic of the file's latest publication names `needle`.
    bool diagnostic_mentions(const std::string& documentUri, std::string_view needle) {
        const auto it = client_.diagnostics.find(documentUri);
        if (it == client_.diagnostics.end()) return false;
        return std::ranges::any_of(it->second, [&](const Json& diagnostic) { return diagnostic.value("message", std::string {}).contains(needle); });
    }

    std::pair<bool, std::string> run_typing(const Json& check, const std::string& file) {
        open(file);
        const std::string documentUri { uri(file) };
        const std::string absolutePath { base::join_path(workspace_, file) };
        const double hz { std::max(0.5, check.value("hz", 10.0)) };
        const auto period = std::chrono::duration<double>(1.0 / hz);
        const std::chrono::seconds duration { check.value("seconds", 120) };
        const std::chrono::milliseconds completionEvery { check.value("completion-every-ms", 1000) };
        const std::chrono::milliseconds autosaveEvery { check.value("autosave-ms", 0) };
        // files.autoSave "afterDelay": the buffer is saved once this long has passed since the last keystroke.
        const std::chrono::milliseconds autosaveIdle { check.value("autosave-idle-ms", 0) };
        const bool autosaving { autosaveEvery.count() > 0 || autosaveIdle.count() > 0 };
        const std::chrono::seconds requestTimeout { check.value("requestTimeout", 65) };
        const Json scripts = check.value("scripts", Json::array());
        if (!scripts.is_array() || scripts.empty()) return { false, "a typing check names no scripts" };
        if (autosaving) remember_original(absolutePath);

        // A second file that imports the typed one, opened with a probe line inside a function body; a completion is asked at the
        // end of that line alongside the typed file's. Only the editor's buffer of it changes: nothing is written to its disk.
        std::string importerFile;
        std::string importerOriginal;
        std::string importerUri;
        Json importerPosition = Json::object();
        if (const auto importer = check.find("importer"); importer != check.end() && importer->is_object()) {
            importerFile = importer->value("file", std::string {});
            if (importerFile.empty()) return { false, "a typing check's importer names no file" };
            importerUri = uri(importerFile);
            open(importerFile);
            importerOriginal = text_of(importerFile);
            std::vector<std::string> importerLines;
            for (const auto line : base::split_lines(importerOriginal)) importerLines.emplace_back(line);
            const std::string probeText { importer->value("text", std::string {}) };
            const std::size_t at { std::min(static_cast<std::size_t>(std::max(0, importer->value("line", 0))), importerLines.size()) };
            importerLines.insert(importerLines.begin() + static_cast<std::ptrdiff_t>(at), probeText);
            std::string probed;
            for (const auto& line : importerLines) probed += line + "\n";
            change(importerFile, probed);
            importerPosition = Json { { "line", at }, { "character", probeText.size() } };
        }
        const bool withImporter { !importerFile.empty() };

        std::vector<std::string> lines;
        {
            const std::string current { text_of(file) };
            for (const auto line : base::split_lines(current)) lines.emplace_back(line);
        }
        const std::vector<std::string> original { lines };
        const auto join_lines = [](const std::vector<std::string>& all) {
            std::string text;
            for (const auto& line : all) text += line + "\n";
            return text;
        };
        std::mt19937_64 rng { static_cast<std::uint64_t>(check.value("seed", 7)) };
        const auto uniform = [&](double low, double high) { return std::uniform_real_distribution<double> { low, high }(rng); };

        struct Cursor {
            int line { 0 };
            int character { 0 };
        } cursor;
        cursor.line = scripts.front().value("line", 0);
        std::vector<std::int64_t> completions;
        std::vector<std::int64_t> importerCompletions;
        auto next_completion = Clock::now() + completionEvery;
        auto next_save = Clock::now() + autosaveEvery;
        std::string savedText { join_lines(lines) };
        auto lastKeystroke { Clock::now() };
        const auto send = [&] {
            lastKeystroke = Clock::now();
            change(file, join_lines(lines));
        };
        const auto save = [&] {
            savedText = join_lines(lines);
            (void)fs::write_file_atomic(absolutePath, savedText);
            client_.notify("textDocument/didSave", Json { { "textDocument", Json { { "uri", documentUri } } } });
            client_.notify("workspace/didChangeWatchedFiles", Json { { "changes", Json::array({ Json { { "uri", documentUri }, { "type", 2 } } }) } });
        };
        // What an editor does while a person types: keystrokes keep coming when an answer is late, and a completion is asked
        // for at the cursor now and then; nothing here waits for an answer.
        const auto elapse = [&](std::chrono::duration<double> span) {
            const auto until { Clock::now() + std::chrono::duration_cast<Clock::duration>(span) };
            for (;;) {
                auto wake { until };
                if (completionEvery.count() > 0) wake = std::min(wake, next_completion);
                if (autosaveEvery.count() > 0) wake = std::min(wake, next_save);
                const bool idleSavePending { autosaveIdle.count() > 0 && join_lines(lines) != savedText };
                if (idleSavePending) wake = std::min(wake, lastKeystroke + autosaveIdle);
                if (wake > Clock::now()) client_.pump_until(wake);
                const auto now { Clock::now() };
                if (completionEvery.count() > 0 && now >= next_completion) {
                    completions.push_back(client_.send_async("textDocument/completion", Json { { "textDocument", Json { { "uri", documentUri } } },
                                                                                             { "position", Json { { "line", cursor.line }, { "character", cursor.character } } } }));
                    if (withImporter) {
                        importerCompletions.push_back(client_.send_async("textDocument/completion", Json { { "textDocument", Json { { "uri", importerUri } } },
                                                                                                         { "position", importerPosition } }));
                    }
                    next_completion += completionEvery;
                }
                if (autosaveEvery.count() > 0 && now >= next_save) {
                    save();
                    next_save += autosaveEvery;
                }
                if (idleSavePending && now >= lastKeystroke + autosaveIdle) save();
                if (now >= until) return;
            }
        };

        // "firstOpenSeconds" (issue #50): an engine may wait for a file's first preamble before it answers a completion
        // in it, as mcppls-clangd does in a module importer. That wait is measured on its own, asking once a second at
        // the cursor until the engine answers one, and the typing below starts after it, so its budgets hold the file
        // as a person edits it, not as it opens.
        std::optional<double> firstOpen;
        const auto firstOpenLimit { budget_number(check, "firstOpenSeconds") };
        if (firstOpenLimit) {
            const auto engineAnswers = [&] {
                long count { 0 };
                for (const auto& [engine, answers] : answered_by(root_report(), "textDocument/completion")) {
                    if (engine != "mcppls") count += answers;
                }
                return count;
            };
            const long answersBefore { engineAnswers() };
            const auto opened { Clock::now() };
            while (seconds_since(opened) < *firstOpenLimit + 30.0 && client_.unusable().empty()) {
                (void)client_.send_async("textDocument/completion", Json { { "textDocument", Json { { "uri", documentUri } } },
                                                                           { "position", Json { { "line", cursor.line }, { "character", cursor.character } } } });
                client_.pump_until(Clock::now() + std::chrono::seconds { 1 });
                if (engineAnswers() > answersBefore) {
                    firstOpen = seconds_since(opened);
                    break;
                }
            }
            client_.pump_until(Clock::now() + std::chrono::milliseconds { 1500 });   // what was still asked is answered before
        }
        const Json before = root_report();
        const std::size_t samplesBefore { client_.statusSamples.size() };
        const auto failedDiagnosticsOf = [&](const std::string& documentUriOf) {
            const auto it = client_.moduleFailedCount.find(documentUriOf);
            return it == client_.moduleFailedCount.end() ? 0 : it->second;
        };
        const int moduleFailedBefore { failedDiagnosticsOf(documentUri) };
        const int importerModuleFailedBefore { withImporter ? failedDiagnosticsOf(importerUri) : 0 };
        const auto began { Clock::now() };
        const auto end { began + duration };
        std::size_t scriptNumber { 0 };
        while (Clock::now() < end && client_.unusable().empty()) {
            const Json& script { scripts[scriptNumber++ % scripts.size()] };
            const std::string text { script.value("text", std::string {}) };
            const std::size_t line { std::min(static_cast<std::size_t>(std::max(0, script.value("line", 0))), lines.size()) };
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(line), std::string {});   // a fresh line to type on
            send();
            cursor = { static_cast<int>(line), 0 };
            for (const char c : text) {
                lines[line].push_back(c);
                ++cursor.character;
                send();
                elapse(period * uniform(0.6, 1.4));
                if (Clock::now() > end) break;
            }
            elapse(std::chrono::duration<double>(uniform(0.5, 2.5)));   // a pause with broken text on the screen
            while (!lines[line].empty()) {
                lines[line].pop_back();
                --cursor.character;
                send();
                elapse(period * 0.5);
            }
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(line));
            cursor = { static_cast<int>(line), 0 };
            send();
            elapse(std::chrono::duration<double>(uniform(0.3, 1.0)));
        }
        // The text is what it was; a saved one is written back before the answers are waited for.
        lines = original;
        send();
        if (autosaving) save();
        if (withImporter) change(importerFile, importerOriginal);
        const auto typingSeconds { seconds_since(began) };

        // The last completions are given until the server's own limit, and no longer: one still unanswered then is a timeout.
        {
            const auto giveUp { Clock::now() + requestTimeout };
            client_.pump_until(Clock::now() + std::chrono::milliseconds { 200 });
            const auto unanswered = [&](std::int64_t id) { return !client_.asyncRequests[id].answered; };
            while (Clock::now() < giveUp && (std::ranges::any_of(completions, unanswered) || std::ranges::any_of(importerCompletions, unanswered))) {
                client_.pump_until(Clock::now() + std::chrono::milliseconds { 200 });
            }
        }
        std::vector<double> answered;
        int timeouts { 0 };
        int errors { 0 };
        for (const auto id : completions) {
            const auto& request { client_.asyncRequests[id] };
            if (!request.answered) ++timeouts;
            else if (request.isError) ++errors;
            else answered.push_back(seconds_between(request.sent, *request.answered));
        }
        std::vector<double> importerAnswered;
        int importerEmpty { 0 };
        for (const auto id : importerCompletions) {
            const auto& request { client_.asyncRequests[id] };
            if (!request.answered) ++timeouts;
            else if (request.isError) ++errors;
            else {
                importerAnswered.push_back(seconds_between(request.sent, *request.answered));
                if (is_empty_result(request.method, request.result)) ++importerEmpty;
            }
        }

        // Diagnostics after the typing stops: a real mistake is typed at once and how long its diagnostic takes is timed.
        std::optional<double> refresh;
        if (const auto probe = check.find("diagnostic-probe"); probe != check.end() && probe->is_object()) {
            auto marked { original };
            const std::size_t at { std::min(static_cast<std::size_t>(std::max(0, probe->value("line", 0))), marked.size()) };
            marked.insert(marked.begin() + static_cast<std::ptrdiff_t>(at), probe->value("text", std::string {}));
            lines = marked;
            const std::string needle { probe->value("expect", std::string {}) };
            const auto typed { Clock::now() };
            send();
            if (autosaving) save();
            const std::chrono::seconds wait { probe->value("wait-seconds", 60) };
            const bool arrived { client_.wait_for([&] {
                const auto it = client_.diagnosticsAt.find(documentUri);
                return it != client_.diagnosticsAt.end() && it->second > typed && diagnostic_mentions(documentUri, needle);
            }, wait) };
            if (arrived) refresh = seconds_since(typed);
            lines = original;
            send();
            if (autosaving) save();
            client_.pump_until(Clock::now() + std::chrono::seconds { 2 });
        }
        const Json after = root_report();
        // The typed file's own: a file taken from clangd is given exactly such a diagnostic, so none means it kept clangd. An
        // importer of a module that does not compile is contained by design (0.0.8 plan M-1), and only counted.
        const int moduleFailedDiagnostics { failedDiagnosticsOf(documentUri) - moduleFailedBefore };
        const int importerModuleFailed { withImporter ? failedDiagnosticsOf(importerUri) - importerModuleFailedBefore : 0 };
        int stalledSamples { 0 };
        for (std::size_t i { samplesBefore }; i < client_.statusSamples.size(); ++i) {
            const auto& issues { client_.statusSamples[i].issues };
            if (std::ranges::find(issues, "preparation-stalled") != issues.end()) ++stalledSamples;
        }
        const long doomed { event_total(after, "file-doomed") - event_total(before, "file-doomed") };
        const Json importerLatency = latency_stats(importerAnswered);

        std::vector<std::string> failures;
        const Json latency = latency_stats(answered);
        enforce_max(check, "p95", latency["p95"].get<double>(), "completion p95", "s", failures);
        enforce_max(check, "max", latency["max"].get<double>(), "completion max", "s", failures);
        const long restarts { event_total(after, "engine-start") - event_total(before, "engine-start") };
        const long setAside { event_total(after, "file-set-aside") - event_total(before, "file-set-aside") };
        const long serverTimeouts { event_total(after, "request-timeout") - event_total(before, "request-timeout") };
        // The two counts are of one thing seen from both sides and are added: the client waits past the server's own limit, so a
        // request the server gave up on is not also a timeout here.
        const long allTimeouts { timeouts + serverTimeouts };
        if (after.is_object() && before.is_object()) {
            enforce_max(check, "restarts", static_cast<double>(restarts), "clangd restarts", "", failures);
            enforce_max(check, "filesSetAside", static_cast<double>(setAside), "files set aside", "", failures);
            enforce_max(check, "doomed", static_cast<double>(doomed), "files doomed", "", failures);
        }
        enforce_max(check, "timeouts", static_cast<double>(allTimeouts), "request timeouts", "", failures);
        enforce_max(check, "moduleFailedDiagnostics", static_cast<double>(moduleFailedDiagnostics), "module-failed diagnostics published for the typed file", "", failures);
        enforce_max(check, "stalled", static_cast<double>(stalledSamples), "status samples with preparation-stalled", "", failures);
        if (withImporter) {
            enforce_max(check, "importerP95", importerLatency["p95"].get<double>(), "importer completion p95", "s", failures);
            enforce_max(check, "importerEmpty", static_cast<double>(importerEmpty), "empty importer completions", "", failures);
        }
        if (const auto limit = budget_number(check, "diagnosticsRefresh")) {
            if (!refresh) failures.push_back(std::format("the diagnostic of the typed mistake never arrived within {} s", check.value("diagnostic-probe", Json::object()).value("wait-seconds", 60)));
            else if (*refresh > *limit) failures.push_back(std::format("diagnostics after typing took {:.2f}s, over the budget {:.2f}s", *refresh, *limit));
        }
        Json detail;
        const auto share { engine_share(before, after, "textDocument/completion", &detail) };
        enforce_min(check, "engineShare", share, "engine share of completion", failures);
        if (firstOpenLimit) {
            if (!firstOpen) failures.push_back(std::format("clangd answered no completion within {:.0f} s of the file opening", *firstOpenLimit + 30.0));
            else if (*firstOpen > *firstOpenLimit) failures.push_back(std::format("clangd's first completion came {:.1f}s after the file opened, over the budget {:.1f}s", *firstOpen, *firstOpenLimit));
        }
        std::set<std::string> statesSeen;
        for (std::size_t i { samplesBefore }; i < client_.statusSamples.size(); ++i) statesSeen.insert(client_.statusSamples[i].state);
        for (const auto& never : check.value("states-never", Json::array())) {
            if (statesSeen.contains(never.get<std::string>())) failures.push_back(std::format("the status was {} while typing", never.get<std::string>()));
        }
        Json summary { { "seconds", typingSeconds }, { "hz", hz }, { "autosaveMs", autosaveEvery.count() }, { "autosaveIdleMs", autosaveIdle.count() }, { "completions", completions.size() },
                       { "answered", answered.size() }, { "clientTimeouts", timeouts }, { "serverTimeouts", serverTimeouts }, { "errors", errors },
                       { "completion", latency }, { "engineShare", share ? Json(*share) : Json(nullptr) }, { "answeredBy", detail },
                       { "restarts", restarts }, { "filesSetAside", setAside }, { "diagnosticsRefresh", refresh ? Json(*refresh) : Json(nullptr) },
                       { "statesSeen", statesSeen }, { "doomed", doomed }, { "moduleFailedDiagnostics", moduleFailedDiagnostics }, { "stalledSamples", stalledSamples },
                       { "firstOpenSeconds", firstOpen ? Json(*firstOpen) : Json(nullptr) } };
        std::string importerBrief;
        if (withImporter) {
            summary["importer"] = Json { { "file", importerFile }, { "completions", importerCompletions.size() }, { "answered", importerAnswered.size() },
                                         { "empty", importerEmpty }, { "completion", importerLatency }, { "moduleFailedDiagnostics", importerModuleFailed } };
            importerBrief = std::format(", importer {} completions p95 {:.2f}s {} empty {} module-failed", importerCompletions.size(), importerLatency["p95"].get<double>(),
                                        importerEmpty, importerModuleFailed);
        }
        return finish_measure(std::move(failures), std::move(summary),
                              std::format("{}{:.0f} s at {:.0f} Hz, {} completions p95 {:.2f}s max {:.2f}s{}, {} restart(s), {} set aside, {} doomed, {} module-failed diagnostic(s), {} stalled sample(s), {} timeout(s), diagnostics after {}",
                                          firstOpen ? std::format("first clangd answer {:.1f}s after opening; then ", *firstOpen) : std::string {},
                                          typingSeconds, hz, completions.size(), latency["p95"].get<double>(), latency["max"].get<double>(), importerBrief, restarts, setAside, doomed,
                                          moduleFailedDiagnostics, stalledSamples, allTimeouts, refresh ? std::format("{:.1f}s", *refresh) : std::string { "never" }));
    }

    // ---- edit-save ----

    std::pair<bool, std::string> run_edit_save(const Json& check) {
        const std::string interfaceFile { check.value("file", std::string {}) };
        const std::string absoluteInterface { base::join_path(workspace_, interfaceFile) };
        const std::string marker { check.value("marker", std::string {}) };
        const std::string inserted { check.value("insert", std::string {}) };
        auto original = fs::read_file(absoluteInterface);
        if (!original) return { false, std::format("{}: {}", interfaceFile, original.error().message) };
        const std::size_t at { original->find(marker) };
        if (marker.empty() || at == std::string::npos) return { false, std::format("{} has no '{}'", interfaceFile, marker) };
        const std::string edited { original->substr(0, at + marker.size()) + inserted + original->substr(at + marker.size()) };
        remember_original(absoluteInterface);

        std::vector<std::string> importers;
        for (const auto& importer : check.value("importers", Json::array())) importers.push_back(importer.get<std::string>());
        if (importers.empty()) return { false, "an edit-save check names no importers" };
        // The first importer carries the mistake whose diagnostic the edit clears: it uses what the interface does not yet declare.
        const Json probe = check.value("probe", Json::object());
        const std::string needle { probe.value("expect", std::string {}) };
        open(importers.front(), text_of(importers.front()) + probe.value("append", std::string {}));
        for (std::size_t i { 1 }; i < importers.size(); ++i) open(importers[i]);
        open(interfaceFile);
        const std::string probeUri { uri(importers.front()) };
        const std::chrono::seconds settleWithin { check.value("settle-seconds", 900) };
        if (!settle(settleWithin)) return { false, "the project did not settle before the edit" };
        if (!needle.empty() && !client_.wait_for([&] { return diagnostic_mentions(probeUri, needle); }, std::chrono::seconds { 60 })) {
            return { false, std::format("{} shows no diagnostic naming '{}' before the edit: the probe measures nothing", importers.front(), needle) };
        }

        std::map<std::string, int> publishedBefore;
        for (const auto& importer : importers) publishedBefore[importer] = client_.diagnosticsCount[uri(importer)];
        const Json before = root_report();
        const std::size_t samplesBefore { client_.statusSamples.size() };
        const std::string interactiveFile { check.value("interactive", Json::object()).value("file", importers.front()) };
        const Json interactiveAt = check.value("interactive", Json::object()).value("at", Json::array({ 0, 0 }));
        const std::chrono::milliseconds interactiveEvery { check.value("interactive", Json::object()).value("every-ms", 1000) };
        const std::chrono::seconds waitFor { check.value("wait-seconds", 120) };

        // The person saves the interface: on disk, in the buffer, and told to the server both ways.
        (void)fs::write_file_atomic(absoluteInterface, edited);
        change(interfaceFile, edited);
        const auto saved { Clock::now() };
        client_.notify("textDocument/didSave", Json { { "textDocument", Json { { "uri", uri(interfaceFile) } } } });
        client_.notify("workspace/didChangeWatchedFiles", Json { { "changes", Json::array({ Json { { "uri", uri(interfaceFile) }, { "type", 2 } } }) } });

        std::vector<std::int64_t> completions;
        std::optional<double> probeCleared;
        std::optional<double> allRepublished;
        auto next_completion { Clock::now() };
        const auto deadline { saved + waitFor };
        while (Clock::now() < deadline && (!allRepublished || (!probeCleared && !needle.empty()))) {
            if (Clock::now() >= next_completion) {
                completions.push_back(client_.send_async("textDocument/completion", Json { { "textDocument", Json { { "uri", uri(interactiveFile) } } }, { "position", position(interactiveAt) } }));
                next_completion += interactiveEvery;
            }
            client_.pump_until(std::min(deadline, Clock::now() + std::chrono::milliseconds { 100 }));
            if (!probeCleared && !needle.empty()) {
                const auto it = client_.diagnosticsAt.find(probeUri);
                if (it != client_.diagnosticsAt.end() && it->second > saved && !diagnostic_mentions(probeUri, needle)) probeCleared = seconds_since(saved);
            }
            if (!allRepublished && std::ranges::all_of(importers, [&](const std::string& importer) { return client_.diagnosticsCount[uri(importer)] > publishedBefore[importer]; })) {
                allRepublished = seconds_since(saved);
            }
        }
        // After the edit the project must be usable again: a jump lands where it should, whatever clangd did meanwhile.
        std::optional<double> recovered;
        if (const auto recover = check.find("recover-probe"); recover != check.end() && recover->is_object()) {
            recovered = time_to_answer(*recover, saved, waitFor);
        }
        {
            const auto giveUp { Clock::now() + std::chrono::seconds { 10 } };
            while (Clock::now() < giveUp && std::ranges::any_of(completions, [&](std::int64_t id) { return !client_.asyncRequests[id].answered; })) {
                client_.pump_until(Clock::now() + std::chrono::milliseconds { 200 });
            }
        }
        std::vector<double> answered;
        int unanswered { 0 };
        for (const auto id : completions) {
            const auto& request { client_.asyncRequests[id] };
            if (request.answered) answered.push_back(seconds_between(request.sent, *request.answered));
            else ++unanswered;
        }
        std::set<std::string> statesSeen;
        int errorSamples { 0 };
        for (std::size_t i { samplesBefore }; i < client_.statusSamples.size(); ++i) {
            statesSeen.insert(client_.statusSamples[i].state);
            if (client_.statusSamples[i].state == "error") ++errorSamples;
        }
        const Json after = root_report();

        // Put the interface back: the next stage starts from the prepared workspace, and what returns is one more change.
        (void)fs::write_file_atomic(absoluteInterface, *original);
        change(interfaceFile, *original);
        client_.notify("textDocument/didSave", Json { { "textDocument", Json { { "uri", uri(interfaceFile) } } } });
        client_.notify("workspace/didChangeWatchedFiles", Json { { "changes", Json::array({ Json { { "uri", uri(interfaceFile) }, { "type", 2 } } }) } });
        const bool restoreSettled { !check.value("settle-after", true) || settle(std::chrono::seconds { check.value("settle-after-seconds", 300) }) };

        std::vector<std::string> failures;
        const Json latency = latency_stats(answered);
        if (allRepublished) enforce_max(check, "republishSeconds", allRepublished, "importers' diagnostics", "s", failures);
        else failures.push_back(std::format("not every importer's diagnostics were published again within {} s", waitFor.count()));
        if (!needle.empty()) {
            if (probeCleared) enforce_max(check, "probeSeconds", probeCleared, "the probe diagnostic", "s", failures);
            else failures.push_back(std::format("the diagnostic naming '{}' never cleared within {} s", needle, waitFor.count()));
        }
        enforce_max(check, "p95", latency["p95"].get<double>(), "completion p95", "s", failures);
        enforce_max(check, "max", latency["max"].get<double>(), "completion max", "s", failures);
        if (unanswered > 0) failures.push_back(std::format("{} completion(s) unanswered", unanswered));
        if (check.contains("recover-probe")) {
            if (recovered) enforce_max(check, "recoverSeconds", recovered, "the probe after the save", "s", failures);
            else failures.push_back(std::format("the probe was never answered as it should be within {} s", waitFor.count()));
        }
        if (const auto limit = budget_number(check, "errorStates"); limit && errorSamples > *limit) failures.push_back(std::format("the status was error {} time(s)", errorSamples));
        if (before.is_object() && after.is_object()) {
            enforce_max(check, "restarts", static_cast<double>(event_total(after, "engine-start") - event_total(before, "engine-start")), "clangd restarts", "", failures);
        }
        Json detail;
        const auto share { engine_share(before, after, "textDocument/completion", &detail) };
        enforce_min(check, "engineShare", share, "engine share of completion", failures);
        if (!restoreSettled) failures.push_back("the project did not settle after the interface was restored");
        Json summary { { "importers", importers.size() }, { "republishSeconds", allRepublished ? Json(*allRepublished) : Json(nullptr) },
                       { "probeSeconds", probeCleared ? Json(*probeCleared) : Json(nullptr) }, { "recoverSeconds", recovered ? Json(*recovered) : Json(nullptr) },
                       { "completion", latency }, { "completionsUnanswered", unanswered }, { "statesSeen", statesSeen }, { "errorSamples", errorSamples },
                       { "restarts", event_total(after, "engine-start") - event_total(before, "engine-start") }, { "engineShare", share ? Json(*share) : Json(nullptr) } };
        const auto text = [](const std::optional<double>& value) { return value ? std::format("{:.1f}s", *value) : std::string { "never" }; };
        return finish_measure(std::move(failures), std::move(summary),
                              std::format("{} importers republished after {}, probe cleared after {}, interactive p95 {:.2f}s max {:.2f}s, jump after {}, states {}", importers.size(),
                                          text(allRepublished), text(probeCleared), latency["p95"].get<double>(), latency["max"].get<double>(), text(recovered), lsp::dump(statesSeen)));
    }

    // ---- fault ----

    struct Injected {
        std::size_t files { 0 };
        std::string what;
    };

    // The lock files a fault wrote, taken away when the check ends: a server left waiting for a lock nobody holds gets it back at
    // once, and the next check does not begin in the state this one broke.
    std::vector<std::string> injectedLocks_;

    // Stale module locks for `module`, as clangd 23.1 leaves them: `.locks/<hash>.lock`, a symlink to `<hash>.lock-<random>`
    // whose content is "hostname pid". `variant` says whose: a dead pid on this host is broken by clangd; a live pid, or
    // another host's, is waited for (up to 260 s a generation, 0.0.6). The module's published files are removed so that it has
    // to be built and its lock is taken.
    std::optional<Injected> inject_lock(const std::string& module, const std::string& variant, std::string& why) {
        const auto published = module_files(cacheDirectory_, module);
        if (published.empty()) {
            why = std::format("no module file of {} in the cache to build again", module);
            return std::nullopt;
        }
        std::set<std::string> sourceDirectories;
        for (const auto& [path, stamp] : published) {
            sourceDirectories.insert(base::parent_path(base::parent_path(path)));
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        const std::string host { variant == "foreign-host" ? std::string { "some-other-host" } : host_name() };
        const std::string pid { variant == "dead-pid" ? std::string { "3999999" } : std::string { "1" } };
        std::size_t created { 0 };
        for (const auto& directory : sourceDirectories) {
            const std::string name { base::file_name(directory) };
            const std::size_t dash { name.rfind('-') };
            if (dash == std::string::npos) continue;
            const std::string hash { name.substr(dash + 1) };
            const std::string locks { base::join_path(base::parent_path(directory), ".locks") };
            (void)fs::create_directories(locks);
            const std::string owner { base::join_path(locks, hash + ".lock-deadbeef") };
            const std::string link { base::join_path(locks, hash + ".lock") };
            if (!fs::write_file(owner, host + " " + pid)) continue;
            std::error_code ignored;
            std::filesystem::remove(link, ignored);
            std::filesystem::create_symlink(owner, link, ignored);
            if (!ignored) {
                ++created;
                injectedLocks_.push_back(owner);
                injectedLocks_.push_back(link);
            }
        }
        if (created == 0) {
            why = "no lock could be written beside the module files";
            return std::nullopt;
        }
        return Injected { created, std::format("{} lock(s) held by {} {}", created, host, pid) };
    }

    std::size_t truncate_module_files(const std::string& module) {
        std::size_t truncated { 0 };
        for (const auto& [path, stamp] : module_files(cacheDirectory_, module)) {
            std::error_code failed;
            const auto size { std::filesystem::file_size(path, failed) };
            if (failed) continue;
            std::filesystem::resize_file(path, size / 2, failed);
            if (!failed) ++truncated;
        }
        return truncated;
    }

    std::pair<bool, std::string> run_fault(const Json& check) {
        const std::string action { check.value("action", std::string {}) };
        const std::chrono::seconds waitFor { check.value("wait-seconds", 120) };
        const Json recoverProbe = check.value("recover-probe", Json::object());
        if (!recoverProbe.contains("at")) return { false, "a fault check names a recover-probe: the request whose right answer shows the project is usable again" };
        struct LockCleanup {
            std::vector<std::string>& files;
            ~LockCleanup() {
                for (const auto& file : files) {
                    std::error_code ignored;
                    std::filesystem::remove(file, ignored);
                }
                files.clear();
            }
        } lockCleanup { injectedLocks_ };
        // What an editor has open when the fault happens: the preparation is of what is open, and a restarted server is given it again.
        for (const auto& one : check.value("open", Json::array())) open(one.get<std::string>());
        open(recoverProbe.value("file", std::string { "src/main.cpp" }));
        std::vector<std::string> failures;
        Json summary { { "action", action } };

        // Where the fault happens: while a preparation is under way ("at-progress"), or once the project is settled.
        const auto arrive = [&]() -> std::optional<std::string> {
            if (check.contains("at-progress")) {
                const double fraction { check.value("at-progress", 0.3) };
                if (!wait_for_progress(fraction, std::chrono::seconds { check.value("progress-timeout", 300) }, std::chrono::seconds { check.value("progress-fallback-seconds", 3) })) {
                    return std::format("the preparation never got {:.0f}% of the way, or was over first", fraction * 100);
                }
            } else if (!settle(std::chrono::seconds { check.value("settle-seconds", 900) })) {
                return std::string { "the project did not settle before the fault" };
            }
            return std::nullopt;
        };
        const auto text = [](const std::optional<double>& value) { return value ? std::format("{:.1f}s", *value) : std::string { "never" }; };

        Clock::time_point t0 { Clock::now() };
        std::string brief;
        long restartsBefore { 0 };
        Json rootBefore;
        std::map<std::string, std::string> pcmBefore;

        if (action == "kill-clangd") {
            if (!watcher_.active()) return { true, "skipped: this platform cannot name clangd's process" };
            const int count { check.value("count", 1) };
            std::vector<double> newClangd;
            long doneAtKill { 0 };
            for (int k { 0 }; k < count; ++k) {
                if (check.contains("at-progress")) {
                    const double fraction { check.value("at-progress", 0.3) + 0.1 * k };
                    if (!wait_for_progress(fraction, std::chrono::seconds { 300 }, std::chrono::seconds { check.value("progress-fallback-seconds", 3) })) return { false, std::format("the preparation never got {:.0f}% of the way, or was over first", fraction * 100) };
                } else if (k == 0) {
                    if (const auto why = arrive()) return { false, *why };
                }
                if (k == 0) {
                    rootBefore = root_report();
                    restartsBefore = event_total(rootBefore, "engine-start");
                }
                const auto pids { watcher_.pids() };
                if (pids.empty()) return { false, "no clangd process is running under the server" };
                doneAtKill = client_.statusSamples.empty() ? 0 : client_.statusSamples.back().done;
                t0 = Clock::now();
                for (const auto pid : pids) (void)kill_process(pid);
                // The server starts another; how long that takes is what a person sees as clangd being gone.
                std::optional<double> replaced;
                while (seconds_since(t0) < 60 && !replaced) {
                    client_.pump_until(Clock::now() + std::chrono::milliseconds { 50 });
                    const auto now { watcher_.pids() };
                    if (std::ranges::any_of(now, [&](std::int64_t pid) { return std::ranges::find(pids, pid) == pids.end(); })) replaced = seconds_since(t0);
                }
                if (replaced) newClangd.push_back(*replaced);
                else failures.push_back(std::format("no new clangd within 60 s of kill {}", k + 1));
                if (k + 1 < count) client_.pump_until(Clock::now() + std::chrono::seconds { 4 });
            }
            summary["newClangdSeconds"] = newClangd;
            if (!newClangd.empty()) enforce_max(check, "newClangdSeconds", *std::ranges::max_element(newClangd), "the slowest new clangd", "s", failures);
            const auto definition { time_to_answer(recoverProbe, t0, waitFor) };
            const bool resumed { client_.wait_for([&] {
                if (state_of(client_.status) == "ready") return true;
                return !client_.statusSamples.empty() && client_.statusSamples.back().done > doneAtKill;
            }, waitFor) };
            (void)client_.wait_for([&] { return ready_seconds_since(t0).has_value(); }, waitFor);
            const auto ready { ready_seconds_since(t0) };
            // How many clangds the server started for the kills it saw: one each is what a kill asks for.
            if (const Json after = root_report(); after.is_object() && rootBefore.is_object()) {
                summary["clangdStarts"] = event_total(after, "engine-start") - restartsBefore;
                enforce_max(check, "clangdStarts", static_cast<double>(event_total(after, "engine-start") - restartsBefore), "clangds started for the kills", "", failures);
            }
            summary["answerSeconds"] = definition ? Json(*definition) : Json(nullptr);
            summary["preparationResumed"] = resumed;
            summary["readySeconds"] = ready ? Json(*ready) : Json(nullptr);
            if (!definition) failures.push_back(std::format("the probe was not answered within {} s of the last kill", waitFor.count()));
            else enforce_max(check, "answerSeconds", definition, "the first answer of the probe", "s", failures);
            if (!resumed) failures.push_back("the preparation did not go on");
            if (!ready) failures.push_back(std::format("the status was not ready again within {} s of the kill", waitFor.count()));
            enforce_max(check, "readySeconds", ready, "ready again", "s", failures);
            brief = std::format("{} kill(s), new clangd after {}, jump after {}, ready {}, preparation {}", count, text(newClangd.empty() ? std::nullopt : std::optional<double> { newClangd.back() }),
                                text(definition), text(ready), resumed ? "went on" : "did not go on");
        } else if (action == "kill-server" || action == "lock" || action == "truncate-pcm") {
            if (action == "kill-server" && !watcher_.active()) return { true, "skipped: this platform cannot name the server's process" };
            if (const auto why = arrive()) return { false, *why };
            pcmBefore = all_module_files(cacheDirectory_);
            tailer_.poll();
            const long lockWaitsBefore { tailer_.count("Still waiting for module lock") };
            const std::string module { check.value("module", std::string {}) };
            std::vector<std::int64_t> clangdBefore { watcher_.pids() };
            std::string why;
            if (action == "kill-server") {
                const std::int64_t server { client_.serverPid.load() };
                if (server <= 0 || !kill_process(server)) return { false, "the server could not be killed" };
                const auto gap { std::chrono::milliseconds { check.value("gap-ms", 0) } };
                if (gap.count() > 0) std::this_thread::sleep_for(gap);
            } else {
                // A lock and a damaged file are found by the server that starts next: this one ends first, as it does when a
                // person closes the editor, and the damage is done to a cache nobody is using.
                client_.stop();
                if (action == "lock") {
                    const auto injected { inject_lock(module, check.value("variant", std::string { "dead-pid" }), why) };
                    if (!injected) return { false, why };
                    summary["injected"] = injected->what;
                } else {
                    const auto truncated { truncate_module_files(module) };
                    if (truncated == 0) return { false, std::format("no module file of {} in the cache to truncate", module) };
                    summary["truncated"] = truncated;
                }
            }
            t0 = Clock::now();
            if (!restart_server(why)) return { false, why };
            const auto definition { time_to_answer(recoverProbe, t0, waitFor) };
            const bool ready { client_.wait_for([&] { return ready_seconds_since(t0).has_value(); }, std::max(std::chrono::seconds { 1 }, waitFor - std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0))) };
            const auto readyAt { ready_seconds_since(t0) };
            const bool preparing { preparing_since(t0) };
            client_.pump_until(Clock::now() + std::chrono::seconds { 3 });
            const Json rootAfter = root_report();
            const std::size_t built { builds_since(pcmBefore, all_module_files(cacheDirectory_)).rebuilt };
            const bool primaryCache { !rootAfter.value("cacheDirectory", std::string {}).contains("/instances/")
                                      && std::ranges::none_of(client_.status.value("notices", Json::array()), [](const Json& notice) { return notice.value("code", std::string {}) == "shared-workspace"; }) };
            long orphans { 0 };
            if (action == "kill-server") {
                // Killed with the server, or left running with nobody to serve? Looked at ten seconds after, and put an end to.
                const auto wait { std::chrono::seconds { 10 } - std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0) };
                if (wait.count() > 0) client_.pump_until(Clock::now() + wait);
                for (const auto pid : clangdBefore) {
                    if (process_alive(pid)) {
                        ++orphans;
                        (void)kill_process(pid);
                    }
                }
                summary["orphanedClangd"] = orphans;
                enforce_max(check, "orphans", static_cast<double>(orphans), "clangd left running", "", failures);
            }
            tailer_.poll();
            const bool logged { tailer_.clangd_output_seen() };
            const long lockWaits { tailer_.count("Still waiting for module lock") - lockWaitsBefore };
            summary["lockWaitLines"] = logged ? Json(lockWaits) : Json(nullptr);
            if (logged) enforce_max(check, "lockWaitLines", static_cast<double>(lockWaits), "module lock waits", "", failures);
            summary["answerSeconds"] = definition ? Json(*definition) : Json(nullptr);
            summary["readySeconds"] = readyAt ? Json(*readyAt) : Json(nullptr);
            summary["preparationResumed"] = preparing;
            summary["newBmi"] = built;
            summary["usesPrimaryCache"] = primaryCache;
            summary["clangdRestarts"] = std::max(0L, event_total(rootAfter, "engine-start") - 1);
            if (!definition) failures.push_back(std::format("the probe was not answered within {} s of the new server", waitFor.count()));
            else enforce_max(check, "answerSeconds", definition, "the first answer of the probe", "s", failures);
            if (!ready) failures.push_back(std::format("the status was not ready within {} s of the new server", waitFor.count()));
            else enforce_max(check, "readySeconds", readyAt, "ready", "s", failures);
            if (definition && ready && readyAt) enforce_max(check, "recoverSeconds", std::max(*definition, *readyAt), "recovery", "s", failures);
            enforce_max(check, "newBmi", static_cast<double>(built), "module files built again", "", failures);
            if (check.value("budget", Json::object()).value("usesPrimaryCache", false) && !primaryCache) failures.push_back("the new server used a private cache instead of the workspace's");
            brief = std::format("new server: jump after {}, ready after {}, {} module file(s) built, {} cache", text(definition), text(readyAt), built,
                                primaryCache ? "the workspace's" : "a private");
        } else if (action == "git-checkout") {
            return run_git_checkout(check, recoverProbe);
        } else {
            return { false, std::format("unknown fault action {}", action) };
        }
        return finish_measure(std::move(failures), std::move(summary), brief);
    }

    // `git checkout` away and back: what a person does between two tasks. The files the checkout changed are told to the
    // server the way an editor's file watcher would; the first leg measures how fast the project is described again, the
    // second (back at the commit the probe is valid at) how fast it is usable.
    std::pair<bool, std::string> run_git_checkout(const Json& check, const Json& recoverProbe) {
        auto git = mcppls::platform::env::find_executable("git");
        if (!git) return { false, "git is not on PATH" };
        const auto run_git = [&](std::vector<std::string> arguments) -> std::optional<std::string> {
            mcppls::platform::SpawnOptions spawn;
            spawn.program = *git;
            spawn.arguments = std::move(arguments);
            spawn.workDirectory = workspace_;
            auto result = mcppls::platform::run(std::move(spawn), std::chrono::minutes { 5 });
            if (!result || result->exitCode != 0) return std::nullopt;
            return result->output;
        };
        const auto head = run_git({ "rev-parse", "HEAD" });
        if (!head) return { false, "the workspace is not a git repository" };
        const std::string home { std::string { base::trim(*head) } };
        const std::string away { check.value("rev", std::string { "HEAD~20" }) };
        const std::chrono::seconds waitFor { check.value("wait-seconds", 300) };
        if (!settle(std::chrono::seconds { check.value("settle-seconds", 900) })) return { false, "the project did not settle before the checkout" };
        std::vector<std::string> failures;
        Json legs = Json::array();
        std::string brief;
        for (int leg { 0 }; leg < 2; ++leg) {
            const std::string target { leg == 0 ? away : home };
            const Json before = root_report();
            // What changes: a name-status list with no renames, so every line is an addition (1), a change (2) or a deletion (3).
            const auto changes = run_git({ "diff", "--name-status", "--no-renames", "HEAD", target });
            if (!changes) return { false, std::format("git cannot compare HEAD with {}", target) };
            Json reported = Json::array();
            std::size_t changed { 0 };
            for (const auto line : base::split_lines(*changes)) {
                const auto tab { line.find('\t') };
                if (tab == std::string_view::npos || tab == 0) continue;
                const int type { line[0] == 'A' ? 1 : line[0] == 'D' ? 3 : 2 };
                const std::string path { base::join_path(workspace_, std::string { line.substr(tab + 1) }) };
                if (client_.watches(path, type)) reported.push_back(Json { { "uri", base::path_to_uri(path) }, { "type", type } });
                ++changed;
            }
            // Open documents keep their text, as they do when a person switches branch with the files unchanged in the editor.
            const auto t0 { Clock::now() };
            const std::size_t samplesBefore { client_.statusSamples.size() };
            if (!run_git({ "checkout", "-q", target })) return { false, std::format("git checkout {} failed", target) };
            for (auto& [path, buffer] : open_) {
                if (auto fresh = fs::read_file(base::join_path(workspace_, path)); fresh && *fresh != buffer.first) change(path, *fresh);
            }
            if (!reported.empty()) client_.notify("workspace/didChangeWatchedFiles", Json { { "changes", reported } });
            // Described again: the status left `ready` for `loading` and came out of it.
            std::optional<double> replanned;
            client_.wait_for([&] {
                bool loading { false };
                for (std::size_t i { samplesBefore }; i < client_.statusSamples.size(); ++i) {
                    if (client_.statusSamples[i].state == "loading") loading = true;
                    else if (loading) {
                        replanned = seconds_between(t0, client_.statusSamples[i].at);
                        return true;
                    }
                }
                return false;
            }, waitFor);
            std::optional<double> definition;
            if (leg == 1) definition = time_to_answer(recoverProbe, t0, waitFor);
            const bool settled { settle(waitFor) };
            const Json after = root_report();
            const long restarts { event_total(after, "engine-start") - event_total(before, "engine-start") };
            Json entry { { "rev", target }, { "filesChanged", changed }, { "replanSeconds", replanned ? Json(*replanned) : Json(nullptr) },
                         { "restarts", restarts }, { "settled", settled }, { "answerSeconds", definition ? Json(*definition) : Json(nullptr) } };
            legs.push_back(entry);
            if (!replanned) failures.push_back(std::format("leg {}: the server never described the project again after the checkout of {}", leg + 1, target));
            else enforce_max(check, "replanSeconds", replanned, std::format("leg {} replan", leg + 1), "s", failures);
            enforce_max(check, "restarts", static_cast<double>(restarts), std::format("leg {} clangd restarts", leg + 1), "", failures);
            if (leg == 1) {
                if (!definition) failures.push_back("the probe was not answered after going back");
                else enforce_max(check, "answerSeconds", definition, "the first answer of the probe after going back", "s", failures);
            }
            brief += std::format("{}{}: {} file(s), replanned {}, {} restart(s)", leg == 0 ? "" : "; ", target, changed, replanned ? std::format("{:.1f}s", *replanned) : std::string { "never" }, restarts);
        }
        return finish_measure(std::move(failures), Json { { "legs", legs } }, brief);
    }

    // ---- timeline ----

    std::pair<bool, std::string> run_timeline(const Json& check) {
        if (const int observe { check.value("observe-seconds", 0) }; observe > 0) client_.pump_until(Clock::now() + std::chrono::seconds { observe });
        const auto now { Clock::now() };
        const auto& samples { client_.statusSamples };

        // The longest time the project was loading, preparing or stalled without a module being prepared: a status that
        // changes state but never advances `progress.done` is not progress. `degraded` and `ready` are settled.
        double longestStall { 0.0 };
        std::optional<Clock::time_point> since { begin_ };
        long lastDone { -1 };
        long lastTotal { -1 };
        const auto close = [&](Clock::time_point at) {
            if (since) longestStall = std::max(longestStall, seconds_between(*since, at));
            since.reset();
        };
        for (const auto& sample : samples) {
            if (sample.state == "restart") {
                close(sample.at);
                since = sample.at;
                lastDone = lastTotal = -1;
                continue;
            }
            const bool waiting { sample.state == "loading" || sample.state == "preparing"
                                 || std::ranges::find(sample.issues, "preparation-stalled") != sample.issues.end() };
            if (!waiting) {
                close(sample.at);
                lastDone = lastTotal = -1;
                continue;
            }
            if (!since) {
                since = sample.at;
                lastDone = sample.done;
                lastTotal = sample.total;
            } else if (sample.done > lastDone || sample.total != lastTotal) {
                close(sample.at);
                since = sample.at;
                lastDone = sample.done;
                lastTotal = sample.total;
            }
        }
        if (since) longestStall = std::max(longestStall, seconds_between(*since, now));

        // State changes in the busiest minute, and the times the status was `error`.
        std::vector<Clock::time_point> changes;
        std::string previous;
        int errors { 0 };
        for (const auto& sample : samples) {
            if (sample.state == "restart") {
                previous.clear();
                continue;
            }
            if (sample.state == "error") ++errors;
            if (!previous.empty() && sample.state != previous) changes.push_back(sample.at);
            previous = sample.state;
        }
        std::size_t busiest { 0 };
        for (std::size_t i { 0 }; i < changes.size(); ++i) {
            std::size_t inWindow { 0 };
            for (std::size_t j { i }; j < changes.size() && changes[j] - changes[i] < std::chrono::seconds { 60 }; ++j) ++inWindow;
            busiest = std::max(busiest, inWindow);
        }

        // Ready, and prepared: the first `ready` that held, and the last one after a preparation, from the start of the run.
        std::optional<double> firstReady;
        std::optional<double> prepared;
        bool sawPreparing { false };
        for (std::size_t i { 0 }; i < samples.size(); ++i) {
            const auto& sample = samples[i];
            if (sample.state == "restart") break;   // the first server's own start is what these are about
            if (sample.state == "preparing") sawPreparing = true;
            if (ready_holds(i)) {
                if (!firstReady) firstReady = seconds_between(begin_, sample.at);
                // The last one: a project whose model is replaced (xlings' transitional model, the producer's answer) is prepared
                // twice, and it is the second time that a person waits for.
                if (sawPreparing) prepared = seconds_between(begin_, sample.at);
                sawPreparing = false;
            }
        }
        const Json root = root_report();
        const long clangdStarts { event_total(root, "engine-start") };
        const long clangdRestarts { std::max(0L, clangdStarts - 1) };

        std::vector<std::string> failures;
        enforce_max(check, "maxStallSeconds", longestStall, "not ready without progress for", "s", failures);
        enforce_max(check, "maxChangesPerMinute", static_cast<double>(busiest), "state changes in a minute", "", failures);
        enforce_max(check, "errorStates", static_cast<double>(errors), "status error", "", failures);
        if (root.is_object()) enforce_max(check, "restarts", static_cast<double>(clangdRestarts), "clangd restarts", "", failures);
        enforce_max(check, "firstReadySeconds", firstReady, "first ready at", "s", failures);
        if (const auto limit = budget_number(check, "preparedSeconds")) {
            if (!prepared) failures.push_back("the preparation never finished");
            else enforce_max(check, "preparedSeconds", prepared, "preparation finished at", "s", failures);
        }
        Json summary { { "statuses", samples.size() }, { "longestStallSeconds", longestStall }, { "stateChanges", changes.size() }, { "busiestMinuteChanges", busiest },
                       { "errorStates", errors }, { "clangdRestarts", root.is_object() ? Json(clangdRestarts) : Json(nullptr) },
                       { "firstReadySeconds", firstReady ? Json(*firstReady) : Json(nullptr) }, { "preparedSeconds", prepared ? Json(*prepared) : Json(nullptr) },
                       { "serverRestarts", client_.restartMarks.size() } };
        const auto number = [](const std::optional<double>& value) { return value ? std::format("{:.1f}s", *value) : std::string { "-" }; };
        return finish_measure(std::move(failures), std::move(summary),
                              std::format("longest stall {:.1f}s, {} state changes ({} in the busiest minute), {} error, clangd restarts {}, first ready {}, prepared {}", longestStall,
                                          changes.size(), busiest, errors, root.is_object() ? std::to_string(clangdRestarts) : std::string { "?" }, number(firstReady), number(prepared)));
    }

    // ---- bmi-reuse ----

    std::pair<bool, std::string> run_bmi_reuse(const Json& check) {
        if (check.value("settle", true) && !settle(std::chrono::seconds { check.value("settle-seconds", 900) })) return { false, "the project did not settle" };
        const auto now { all_module_files(cacheDirectory_) };
        if (pcmBefore_.empty()) {
            measure_ = Json { { "cold", true }, { "moduleFiles", now.size() } };
            return { !expectWarm_, std::format("a cold start: {} module file(s) were built and none was there before", now.size()) };
        }
        const Builds builds { builds_since(pcmBefore_, now) };
        tailer_.poll();
        const bool logged { tailer_.clangd_output_seen() };
        // Every module built logs one line; those of modules built for the first time are not rebuilds.
        const long lines { std::max(0L, tailer_.count("Built module") - static_cast<long>(builds.first)) };
        std::vector<std::string> failures;
        enforce_max(check, "newBmi", static_cast<double>(builds.rebuilt), "module files built again", "", failures);
        // Log lines only count where the log carries clangd's own output: at the default level it does not, and 0 would say nothing.
        if (logged) enforce_max(check, "builtLines", static_cast<double>(lines), "'Built module' lines of modules built before", "", failures);
        Json summary { { "moduleFilesBefore", pcmBefore_.size() }, { "moduleFilesNow", now.size() }, { "newBmi", builds.rebuilt },
                       { "firstBuilds", builds.first }, { "builtModuleLines", logged ? Json(lines) : Json(nullptr) } };
        return finish_measure(std::move(failures), std::move(summary),
                              std::format("{} of {} module file(s) built again, {} module(s) built for the first time{}", builds.rebuilt, pcmBefore_.size(),
                                          builds.first,
                                          logged ? std::format(", {} 'Built module' line(s) of modules built before", lines) : std::string { ", the log carries no clangd output (--log-level debug)" }));
    }

    // ---- resources ----

    std::pair<bool, std::string> run_resources(const Json& check) {
        std::vector<std::string> failures;
        Json summary = Json::object();
        std::string brief;
        std::optional<double> idleCores;
        std::optional<double> growth;
        if (const int idle { check.value("idle-seconds", 0) }; idle > 0) {
            if (!settle(std::chrono::seconds { check.value("settle-seconds", 900) })) return { false, "the project did not settle before the idle period" };
            // A clangd that has just gone quiet has not yet loaded what it will keep: the window starts after a warm-up minute, or
            // its growth would be measured against a process that is not full-sized yet.
            client_.pump_until(Clock::now() + std::chrono::seconds { check.value("warmup-seconds", 60) });
            const auto rssBefore { watcher_.rss_mb_now() };
            const auto from { Clock::now() };
            client_.pump_until(from + std::chrono::seconds { idle });
            idleCores = watcher_.cpu_cores(from, Clock::now());
            const auto rssAfter { watcher_.rss_mb_now() };
            if (rssBefore && rssAfter && *rssBefore > 0) growth = (*rssAfter - *rssBefore) / *rssBefore;
            summary["idleSeconds"] = idle;
            summary["idleCpuCores"] = idleCores ? Json(*idleCores) : Json(nullptr);
            summary["rssGrowth"] = growth ? Json(*growth) : Json(nullptr);
            summary["rssBeforeMB"] = rssBefore ? Json(*rssBefore) : Json(nullptr);
            summary["rssAfterMB"] = rssAfter ? Json(*rssAfter) : Json(nullptr);
            enforce_max(check, "idleCpuCores", idleCores, "clangd's CPU while nobody asked, in cores,", "", failures);
            enforce_max(check, "rssGrowth", growth, "clangd's memory growth", "", failures);
            brief = std::format("idle {} s: clangd {} core(s), memory {} ", idle, idleCores ? std::format("{:.3f}", *idleCores) : std::string { "?" },
                                growth ? std::format("{:+.1f}%", *growth * 100.0) : std::string { "?" });
        }
        const auto peak { watcher_.peak_rss_mb() };
        summary["peakRssMB"] = peak ? Json(*peak) : Json(nullptr);
        enforce_max(check, "rssMB", peak, "clangd's peak resident memory", "MB", failures);
        brief += peak ? std::format("peak RSS {:.0f} MB", *peak) : std::string { "peak RSS unknown here" };
        return finish_measure(std::move(failures), std::move(summary), brief);
    }


    // ---- graph-edit ----

    bool graph_has(const std::string& module) {
        auto answer = client_.request("cxxModules/graph", Json::object(), std::chrono::seconds { 20 });
        if (!answer || !answer->is_object()) return false;
        return std::ranges::any_of(answer->value("modules", Json::array()), [&](const Json& one) { return one.value("name", std::string {}) == module; });
    }

    // What an editor's file watcher does for a change on disk; with nothing registered (--no-dynamic-watch) the server's own polling has to notice.
    void report_file_change(const std::string& path, int type) {
        const std::string canonical { fs::canonical_path(path) };
        if (client_.watches(path, type) || (!canonical.empty() && client_.watches(canonical, type))) {
            client_.notify("workspace/didChangeWatchedFiles", Json { { "changes", Json::array({ Json { { "uri", base::path_to_uri(path) }, { "type", type } } }) } });
        }
    }

    // Seconds from `t0` until the server described the project again after a change: the status went to `loading` and came out
    // of it. nullopt when it never did within `wait`.
    std::optional<double> replanned_since(Clock::time_point t0, std::size_t samplesBefore, std::chrono::seconds wait) {
        std::optional<double> replanned;
        client_.wait_for([&] {
            bool loading { false };
            for (std::size_t i { samplesBefore }; i < client_.statusSamples.size(); ++i) {
                if (client_.statusSamples[i].state == "loading") {
                    loading = true;
                } else if (loading) {
                    replanned = seconds_between(t0, client_.statusSamples[i].at);
                    return true;
                }
            }
            return false;
        }, wait);
        return replanned;
    }

    std::set<std::string> createdOnFinish_;   // files a graph-edit added, removed when the run ends

    std::pair<bool, std::string> run_graph_edit(const Json& check) {
        const std::string action { check.value("action", std::string {}) };
        const std::string file { check.value("file", std::string {}) };
        const std::string path { base::join_path(workspace_, file) };
        const std::string module { check.value("module", std::string {}) };
        const std::chrono::seconds waitFor { check.value("wait-seconds", 60) };
        const std::chrono::seconds quiet { check.value("quiet-seconds", 10) };
        std::vector<std::string> failures;
        Json summary { { "action", action } };
        std::string brief;
        const Json before = root_report();
        const auto text = [](const std::optional<double>& value) { return value ? std::format("{:.1f}s", *value) : std::string { "never" }; };

        if (action == "add" || action == "remove") {
            const bool adding { action == "add" };
            if (adding) {
                (void)fs::create_directories(base::parent_path(path));
                const bool existed { fs::exists(path) };
                if (!existed) createdOnFinish_.insert(path);
                else remember_original(path);
                const std::string content { check.value("content", std::format("export module {};\nimport std;\nexport int ux_value() {{ return 1; }}\n", module)) };
                if (auto written = fs::write_file(path, content); !written) return { false, written.error().message };
                report_file_change(path, existed ? 2 : 1);
            } else {
                if (!fs::exists(path)) return { false, std::format("{} does not exist", file) };
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                createdOnFinish_.erase(path);
                report_file_change(path, 3);
            }
            const auto t0 { Clock::now() };
            std::optional<double> graphSeconds;
            while (seconds_since(t0) < static_cast<double>(waitFor.count()) && !graphSeconds) {
                if (graph_has(module) == adding) graphSeconds = seconds_since(t0);
                else client_.pump_until(Clock::now() + std::chrono::milliseconds { 250 });
            }
            client_.pump_until(Clock::now() + quiet);
            const Json after = root_report();
            const long restarts { event_total(after, "engine-start") - event_total(before, "engine-start") };
            summary["graphSeconds"] = graphSeconds ? Json(*graphSeconds) : Json(nullptr);
            summary["restarts"] = restarts;
            if (!graphSeconds) failures.push_back(std::format("the module graph {} {} within {} s", adding ? "never listed" : "still lists", module, waitFor.count()));
            else enforce_max(check, "graphSeconds", graphSeconds, "the module graph", "s", failures);
            if (before.is_object() && after.is_object()) enforce_max(check, "restarts", static_cast<double>(restarts), "clangd restarts", "", failures);
            brief = std::format("{} {}: the graph followed after {}, {} restart(s)", adding ? "added" : "removed", file, text(graphSeconds), restarts);
        } else if (action == "config") {
            // A build input changes and comes back: the project is described again both times, the first time with new commands
            // (BMIs are built for them: not measured), the second time with commands it has seen, so nothing is built again.
            const Json replace = check.value("replace", Json::object());
            const std::string from { replace.value("from", std::string {}) };
            remember_original(path);
            auto original = fs::read_file(path);
            if (!original || from.empty() || !original->contains(from)) return { false, std::format("{} has no '{}'", file, from) };
            const std::string changed { base::replace_all(*original, from, replace.value("with", std::string {})) };
            if (!settle(std::chrono::seconds { check.value("settle-seconds", 900) })) return { false, "the project did not settle before the change" };
            std::size_t samplesBefore { client_.statusSamples.size() };
            auto t0 { Clock::now() };
            (void)fs::write_file(path, changed);
            report_file_change(path, 2);
            const auto changeSeconds { replanned_since(t0, samplesBefore, waitFor) };
            const bool settledAfterChange { settle(std::chrono::seconds { check.value("settle-seconds", 900) }) };
            const auto pcmBeforeRevert { all_module_files(cacheDirectory_) };
            samplesBefore = client_.statusSamples.size();
            const Json beforeRevert = root_report();
            t0 = Clock::now();
            (void)fs::write_file(path, *original);
            report_file_change(path, 2);
            const auto revertSeconds { replanned_since(t0, samplesBefore, waitFor) };
            const bool settledAfterRevert { settle(std::chrono::seconds { check.value("settle-seconds", 900) }) };
            const std::size_t rebuilt { builds_since(pcmBeforeRevert, all_module_files(cacheDirectory_)).rebuilt };
            const Json after = root_report();
            summary["changeSeconds"] = changeSeconds ? Json(*changeSeconds) : Json(nullptr);
            summary["revertSeconds"] = revertSeconds ? Json(*revertSeconds) : Json(nullptr);
            summary["revertNewBmi"] = rebuilt;
            summary["revertRestarts"] = event_total(after, "engine-start") - event_total(beforeRevert, "engine-start");
            if (!changeSeconds) failures.push_back("the server never described the project again after the change");
            else enforce_max(check, "changeSeconds", changeSeconds, "the project described again after the change", "s", failures);
            if (!revertSeconds) failures.push_back("the server never described the project again after the change was undone");
            else enforce_max(check, "revertSeconds", revertSeconds, "the project described again after the change was undone", "s", failures);
            enforce_max(check, "revertNewBmi", static_cast<double>(rebuilt), "module files built again after the change was undone", "", failures);
            if (!settledAfterChange || !settledAfterRevert) failures.push_back("the project did not settle");
            brief = std::format("changed {}: described again after {}; undone: after {}, {} module file(s) built again", file, text(changeSeconds), text(revertSeconds), rebuilt);
        } else {
            return { false, std::format("unknown graph-edit action {}", action) };
        }
        return finish_measure(std::move(failures), std::move(summary), brief);
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
        if (auto retired = retired_canary(check)) return { true, *retired };
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
                // 0.0.10 plan C-13.1: the cache's own fill level, `ok` | `near` | `over`, inside the
                // optional `cache` field. Absent from the status means the check fails, which is the
                // point: a server that stops carrying the field is caught here.
                if (auto cacheState = check.find("cache-state"); cacheState != check.end()) {
                    matched = matched && snapshot.value("cache", Json::object()).value("state", std::string {}) == cacheState->get<std::string>();
                }
                if (auto issueCode = check.find("issue-code"); issueCode != check.end()) {
                    const std::string wantedCommand { check.value("issue-command", std::string {}) };
                    const std::string wantedMessage { check.value("issue-message", std::string {}) };   // a part of the message
                    const std::string wantedCategory { check.value("issue-category", std::string {}) };   // S3: code | engine | environment | project
                    // S3-4-16 (plan 2026-09-27 B-2): whether the issue lets a client offer to fetch what is missing.
                    const std::optional<bool> wantedAskOnline { check.contains("issue-ask-online") ? std::optional { check.value("issue-ask-online", false) }
                                                                                                    : std::nullopt };
                    // K-7 (plan 2026-09-30): the issue names the diagnostic bundle written for it, and the file is there.
                    const bool wantedBundle { check.value("issue-bundle", false) };
                    matched = matched && std::ranges::any_of(snapshot.value("issues", Json::array()), [&](const Json& issue) {
                        if (issue.value("code", std::string {}) != issueCode->get<std::string>()) return false;
                        if (wantedBundle && (!issue.contains("bundle") || !fs::is_regular_file(issue.value("bundle", std::string {})))) return false;
                        if (wantedAskOnline && issue.value("askOnline", false) != *wantedAskOnline) return false;
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
                // D-5 (plan 0.0.9): how the last fetch asked for ended (S3 onlineRun), and a part of its message.
                if (auto outcome = check.find("online-run"); outcome != check.end()) {
                    const Json run = snapshot.value("onlineRun", Json::object());
                    matched = matched && run.is_object() && run.value("outcome", std::string {}) == outcome->get<std::string>()
                              && run.value("message", std::string {}).contains(check.value("online-run-message", std::string {}))
                              && !run.value("at", std::string {}).empty();
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
            apply_path_prepend(environment, options_);
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
            const std::string clangd { clangd_program() };
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
            // The defect shows as a hang on Linux and macOS, and as a crash on Windows (0x80000003): either is clangd not
            // finishing. A normal exit, with or without errors, is --check's 0 to 3.
            const bool crashed { !result->timedOut && (result->exitCode < 0 || result->exitCode > 125) };
            const bool hung { result->timedOut || crashed };
            const std::string expected { check.value("expect", std::string { "hangs" }) };
            const std::string says { check.value("says", std::string {}) };
            if (expected == "hangs" && !hung) {
                return { false, std::format("{} (clangd exited {} in under {} s: {})", says.empty() ? std::string { "clangd finished; the defect is gone" } : says,
                                            result->exitCode, limit.count(), (result->output + result->error).substr(0, 200)) };
            }
            if (expected == "finishes" && hung) return { false, std::format("clangd did not finish {} in {} s", file, limit.count()) };
            if (crashed) return { true, std::format("clangd crashed on {} (exit {}), as expected", file, result->exitCode) };
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
            std::set<std::string> sizesBefore;   // an owned read copy holds the bytes of a payload there was
            for (const auto& [path, stamp] : before->second) {
                if (owned_payload(path)) sizesBefore.insert(stamp.substr(0, stamp.find(':')));
            }
            for (const auto& [path, stamp] : now) {
                if (before->second.contains(path)) continue;
                if (owned_payload(path) && sizesBefore.contains(stamp.substr(0, stamp.find(':')))) continue;
                differences.push_back("added " + path);
            }
            return { differences.empty(), differences.empty() ? std::format("{} file(s) of {} reused", before->second.size(), module) : lsp::dump(differences) };
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
        if (kind == "status-never") {
            // 0.0.8 part 2 (X-6): every status the server has sent so far, not only the latest: none names an issue whose code
            // is in `issue-codes` (the provisional model of a trusted workspace once said "untrusted-workspace" for a moment).
            // `after-ready` waits for a first `ready` so that the provisional model's statuses are among those looked at.
            if (check.value("after-ready", true)) (void)client_.wait_for([&] { return client_.firstReady.has_value(); }, timeout_);
            const auto codes { check.value("issue-codes", std::vector<std::string> {}) };
            std::set<std::string> seen;
            for (const auto& sample : client_.statusSamples) {
                for (const auto& issue : sample.issues) {
                    if (std::ranges::find(codes, issue) != codes.end()) seen.insert(issue);
                }
            }
            return { seen.empty(), seen.empty() ? std::format("none of {} in {} statuses", base::join(codes, ", "), client_.statusSamples.size())
                                                : std::format("seen: {}", base::join(std::vector<std::string>(seen.begin(), seen.end()), ", ")) };
        }
        if (kind == "engine-command") {
            // 0.0.8 part 2 (X-7): the command clangd was given for `file`, read from the database the server wrote for it
            // (under the cache directory: `contexts/default/cdb/compile_commands.json`, the newest when there are several).
            // Each of `contains` is an argument of it, none of `absent` is; retried within the check's time, since the
            // database follows the model.
            const auto wanted = [&](const char* key) {
                std::vector<std::string> names;
                if (const auto it = check.find(key); it != check.end()) {
                    if (it->is_string()) names.push_back(it->get<std::string>());
                    else for (const auto& name : *it) names.push_back(name.get<std::string>());
                }
                return names;
            };
            const auto contains { wanted("contains") };
            const auto absent { wanted("absent") };
            const auto deadline = Clock::now() + timeout_;
            std::string detail { "no engine database yet" };
            do {
                std::string newest;
                std::int64_t newestTime { -1 };
                for (const auto& candidate : fs::list_files(cacheDirectory_, std::array<std::string_view, 1> { ".json" }, {})) {
                    if (base::file_name(candidate) != "compile_commands.json" || !candidate.contains("/contexts/default/cdb/")) continue;
                    if (const auto stamp = fs::stamp(candidate); stamp && stamp->modified > newestTime) {
                        newest = candidate;
                        newestTime = stamp->modified;
                    }
                }
                if (!newest.empty()) {
                    const auto text = fs::read_file(newest);
                    const Json database = text ? Json::parse(*text, nullptr, false) : Json();
                    detail = std::format("no command for {} in {}", file, newest);
                    for (const auto& entry : database.is_array() ? database : Json::array()) {
                        if (!entry.value("file", std::string {}).ends_with(file)) continue;
                        std::vector<std::string> arguments;
                        if (entry.contains("arguments")) arguments = entry["arguments"].get<std::vector<std::string>>();
                        const bool ok { std::ranges::all_of(contains, [&](const std::string& name) { return std::ranges::find(arguments, name) != arguments.end(); })
                                        && std::ranges::none_of(absent, [&](const std::string& name) { return std::ranges::find(arguments, name) != arguments.end(); }) };
                        if (ok) return { true, base::join(arguments, " ").substr(0, 240) };
                        detail = base::join(arguments, " ").substr(0, 400);
                    }
                }
                client_.drain(std::chrono::milliseconds { 300 });
            } while (Clock::now() < deadline);
            return { false, detail };
        }
        if (kind == "write-midway") {
            // 0.0.8 part 2 (X-5): a tool rewriting a database in place. `file` is cut to its first `cut` (0.5) part, which is
            // not valid JSON, `after-ms` (300) later written complete -- with `replace` ({"from", "with"}) applied to it, so
            // that a model which followed shows it. The server is told of both writes, as an editor's watcher would.
            const std::string path { base::join_path(workspace_, file) };
            remember_original(path);
            auto current = fs::read_file(path);
            if (!current) return { false, std::format("{}: {}", file, current.error().message) };
            std::string complete { *current };
            if (const auto replace = check.find("replace"); replace != check.end() && replace->is_object()) {
                const std::string from { replace->value("from", std::string {}) };
                if (from.empty() || !complete.contains(from)) return { false, std::format("{} has no '{}'", file, from) };
                complete = base::replace_all(complete, from, replace->value("with", std::string {}));
            }
            const std::string canonical { fs::canonical_path(path) };
            const auto write = [&](const std::string& content) -> base::Result<void> {
                auto written = fs::write_file(path, content);
                if (written && (client_.watches(path, 2) || client_.watches(canonical, 2))) {
                    client_.notify("workspace/didChangeWatchedFiles", Json { { "changes", Json::array({ Json { { "uri", base::path_to_uri(path) }, { "type", 2 } } }) } });
                }
                return written;
            };
            const auto cut = static_cast<std::size_t>(static_cast<double>(complete.size()) * check.value("cut", 0.5));
            if (auto written = write(complete.substr(0, cut)); !written) return { false, written.error().message };
            // A fixed time, the writer's: drain's "until quiet" never ended while the server re-read the file each second.
            client_.pump_for(std::chrono::milliseconds { check.value("after-ms", 300) });
            if (auto written = write(complete); !written) return { false, written.error().message };
            return { true, std::format("{} cut at {} of {} bytes, complete again", file, cut, complete.size()) };
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
            // 0.0.7 scenario tests: `replace` ({"from": "...", "with": "..."}) rewrites part of the file as it is, and `restore`
            // writes back what it was before the first such change, or before the run began (the run also does it when it ends).
            if (check.value("restore", false)) {
                const auto kept = restoreOnFinish_.find(path);
                if (kept == restoreOnFinish_.end()) return { false, std::format("{}: nothing was changed, nothing to restore", file) };
                content = kept->second;
                restoreOnFinish_.erase(kept);
            } else if (const auto replace = check.find("replace"); replace != check.end() && replace->is_object()) {
                remember_original(path);
                auto current = fs::read_file(path);
                if (!current) return { false, std::format("{}: {}", file, current.error().message) };
                const std::string from { replace->value("from", std::string {}) };
                if (from.empty() || !current->contains(from)) return { false, std::format("{} has no '{}'", file, from) };
                content = base::replace_all(*current, from, replace->value("with", std::string {}));
            }
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
            // "notify": true is an editor's own watcher of build files (the VS Code client's), which reports the write whether or not
            // the server registered anything: an inferred model, kept while a build tool cannot answer, registers nothing.
            if (check.value("notify", false) || client_.watches(path, type) || client_.watches(canonical, type)) {
                client_.notify("workspace/didChangeWatchedFiles", Json { { "changes", Json::array({ Json { { "uri", base::path_to_uri(path) }, { "type", type } } }) } });
            }
            // G-5 (plan 2026-09-30): "expect-reload": false is a change that must NOT load the model again (an edit that
            // leaves every module's structure as it was), watched for "settle" seconds (default 10).
            if (check.contains("expect-reload") && !check.value("expect-reload", true)) {
                const auto settle = std::chrono::seconds { check.value("settle", 10) };
                const bool reloaded { client_.wait_for([&] {
                    const auto& states = history();
                    return states.size() > seen && std::ranges::find(states.begin() + static_cast<std::ptrdiff_t>(seen), states.end(), "loading") != states.end();
                }, settle) };
                return { !reloaded, reloaded ? std::format("{}: the model loaded again", file) : std::format("{}: no reload in {} s, as expected", file, settle.count()) };
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
            // overall design 7.7: a command the server declared, answered without an error. "{workspace-uri}" in an
            // argument is the workspace folder's URI as this client sent it (not canonical: /tmp is /private/tmp on macOS).
            Json arguments = check.value("arguments", Json::array());
            const std::function<void(Json&)> expand = [&](Json& value) {
                if (value.is_string()) value = base::replace_all(value.get<std::string>(), "{workspace-uri}", base::path_to_uri(workspace_));
                else if (value.is_array() || value.is_object()) {
                    for (auto& item : value) expand(item);
                }
            };
            expand(arguments);
            const auto answer = client_.request("workspace/executeCommand",
                                                Json { { "command", check.value("command", std::string {}) }, { "arguments", std::move(arguments) } },
                                                timeout_);
            // "expect": fields the answer must carry with these values (an error answers nothing).
            bool expected { answer.has_value() };
            if (const auto expect = check.find("expect"); expected && expect != check.end() && expect->is_object()) {
                for (const auto& item : expect->items()) {
                    expected = expected && answer->is_object() && answer->contains(item.key()) && (*answer)[item.key()] == item.value();
                }
            }
            return { expected, answer ? lsp::dump(*answer) : std::string { "no answer, or an error" } };
        }
        if (kind == "diagnostic-code-lines") {
            // I-1 (plan 0.0.8 part 2): the lines (0-based) a code is on once the file's diagnostics settle, exactly: include
            // cleaner in module units says only what is true, and a clangd that starts saying more is caught.
            open(file);
            const std::string documentUri { uri(file) };
            const Json code = check.value("code", std::string {});
            const bool published { client_.wait_for([&] {
                return client_.diagnosticsCount[documentUri] > 0 && state_of(client_.status) != "preparing" && state_of(client_.status) != "loading";
            }, timeout_) };
            client_.drain(std::chrono::milliseconds { 1500 });
            std::vector<int> lines;
            for (const auto& diagnostic : client_.diagnostics[documentUri]) {
                if (diagnostic.value("code", Json {}) != code) continue;
                const Json* start { lsp::find_path(diagnostic, { "range", "start" }) };
                lines.push_back(start == nullptr ? -1 : start->value("line", -1));
            }
            std::ranges::sort(lines);
            std::vector<int> expected { check.value("lines", std::vector<int> {}) };
            std::ranges::sort(expected);
            // "subset": no line outside `lines` (a clangd that reports fewer is not wrong; one that reports more is).
            const bool held { check.value("subset", false) ? std::ranges::includes(expected, lines) : lines == expected };
            return { published && held, published ? std::format("{} on lines {}", code.dump(), lines) : std::string { "no diagnostics were published" } };
        }
        if (kind == "diagnostic-code") {
            open(file);
            const std::string documentUri { uri(file) };
            const std::string code { check.value("expect", std::string {}) };
            // Fix plan F11, F12: where the diagnostic is (`line`, 0-based), how severe (`severity`), and which codes
            // must not be there with it (`absent`).
            const std::optional<int> line { check.contains("line") ? std::optional<int> { check.value("line", 0) } : std::nullopt };
            const std::optional<int> severity { check.contains("severity") ? std::optional<int> { check.value("severity", 1) } : std::nullopt };
            const Json absent = check.value("absent", Json::array());
            const bool found { client_.wait_for([&] {
                bool hit { false };
                for (const auto& diagnostic : client_.diagnostics[documentUri]) {
                    const Json& diagnosticCode { diagnostic.value("code", Json {}) };
                    if (std::ranges::find(absent, diagnosticCode) != absent.end()) return false;
                    if (diagnosticCode != Json(code)) continue;
                    const Json* start { lsp::find_path(diagnostic, { "range", "start" }) };
                    if (line && (start == nullptr || start->value("line", -1) != *line)) continue;
                    if (severity && diagnostic.value("severity", 1) != *severity) continue;
                    hit = true;
                }
                return hit;
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
        if (kind == "formatting-equals") {
            open(file);
            const std::string original { text_of(file) };
            const std::string expected { check.contains("expected-file")
                ? text_of(check.at("expected-file").get<std::string>())
                : check.value("expect", std::string {}) };
            if (original.empty() || expected.empty()) return { false, "formatting requires nonempty input and golden" };
            auto apply = [&](const Json& edits) -> std::optional<std::string> {
                if (!edits.is_array()) return std::nullopt;
                struct Edit { std::size_t begin; std::size_t end; std::string text; };
                std::vector<Edit> changes;
                for (const auto& edit : edits) {
                    const auto& start = edit.at("range").at("start");
                    const auto& end = edit.at("range").at("end");
                    auto from = base::offset_at(original, { start.at("line").get<int>(), start.at("character").get<int>() });
                    auto to = base::offset_at(original, { end.at("line").get<int>(), end.at("character").get<int>() });
                    if (!from || !to || *from > *to) return std::nullopt;
                    changes.push_back({ *from, *to, edit.at("newText").get<std::string>() });
                }
                std::ranges::sort(changes, std::greater {}, &Edit::begin);
                std::string formatted { original };
                std::size_t boundary { original.size() };
                for (const auto& edit : changes) {
                    if (edit.end > boundary) return std::nullopt;
                    formatted.replace(edit.begin, edit.end - edit.begin, edit.text);
                    boundary = edit.begin;
                }
                return formatted;
            };
            auto [ok, result] = retry("textDocument/formatting", [&] {
                return Json { { "textDocument", Json { { "uri", uri(file) } } },
                              { "options", Json { { "tabSize", 4 }, { "insertSpaces", true } } } };
            }, [&](const Json& edits) { return apply(edits) == std::optional<std::string> { expected }; });
            const auto formatted = apply(result);
            return { ok, formatted ? *formatted : "invalid formatting edits" };
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
            // "expect": a label prefix, or several that must all be there ("exact": whole labels); "absent": labels that must not be.
            const bool exact { check.value("exact", false) };
            std::vector<std::string> expected;
            if (const auto wanted = check.find("expect"); wanted != check.end() && wanted->is_array()) {
                for (const auto& one : *wanted) expected.push_back(one.get<std::string>());
            } else {
                expected.push_back(check.value("expect", std::string {}));
            }
            std::vector<std::string> absent;
            for (const auto& one : check.value("absent", Json::array())) absent.push_back(one.get<std::string>());
            auto [ok, result] = retry("textDocument/completion", [&] { return completion_params(check, file); },
                [&](const Json& value) {
                    const auto labels = completion_labels(value);
                    const auto present = [&](const std::string& prefix) {
                        return std::ranges::any_of(labels, [&](const std::string& label) { return exact ? label == prefix : label.starts_with(prefix); });
                    };
                    return std::ranges::all_of(expected, present)
                           && std::ranges::none_of(absent, [&](const std::string& label) { return std::ranges::find(labels, label) != labels.end(); });
                });
            auto labels = completion_labels(result);
            if (labels.size() > 12) labels.resize(12);
            return { ok, lsp::dump(labels) };
        }
        if (kind == "completion-empty") {
            // F9 (fix plan 2026-09-26, D4): a completion answered with no items, within "within-ms" when given --
            // a space typed outside an import line is answered at once, without asking the core engine.
            open(file);
            const auto start = Clock::now();
            auto answer = client_.request("textDocument/completion", completion_params(check, file), timeout_);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            if (!answer) return { false, "no answer" };
            const bool empty { completion_labels(*answer).empty() };
            const bool inTime { !check.contains("within-ms") || elapsed <= check.value("within-ms", std::int64_t { 0 }) };
            return { empty && inTime, std::format("{} in {} ms", lsp::dump(*answer).substr(0, 120), elapsed) };
        }
        if (kind == "capabilities") {
            // The server capabilities initialize answered with, held to "expect" like a tool's result.
            auto [held, detail] = expectations_hold(capabilities_, check.value("expect", Json::array()));
            return { held, held ? lsp::dump(capabilities_.value("completionProvider", Json::object())).substr(0, 160) : detail };
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
        if (kind == "semantic-tokens") {
            // design doc 2026-09-25 K/§7: every entry of "expect" ({"line", "text", "type",
            // "modifiers"?}) must be one of the decoded tokens; "modifiers" (a list) is optional.
            open(file);
            const std::string content { text_of(file) };
            const Json expected = check.value("expect", Json::array());
            const auto range = check.find("range");
            const std::string method { range != check.end() ? std::string { "textDocument/semanticTokens/range" } : std::string { "textDocument/semanticTokens/full" } };
            auto [ok, result] = retry(method,
                [&] {
                    Json params { { "textDocument", Json { { "uri", uri(file) } } } };
                    if (range != check.end()) params["range"] = *range;
                    return params;
                },
                [&](const Json& value) {
                    const auto decoded = decode_semantic_tokens(value, content);
                    return std::ranges::all_of(expected, [&](const Json& want) {
                        const int line { want.value("line", -1) };
                        const std::string text { want.value("text", std::string {}) };
                        const std::string type { want.value("type", std::string {}) };
                        std::vector<std::string> modifiers;
                        for (const auto& modifier : want.value("modifiers", Json::array())) modifiers.push_back(modifier.get<std::string>());
                        return std::ranges::any_of(decoded, [&](const DecodedToken& token) {
                            return token.line == line && token.text == text && token.type == type
                                && std::ranges::all_of(modifiers, [&](const std::string& modifier) { return std::ranges::find(token.modifiers, modifier) != token.modifiers.end(); });
                        });
                    });
                });
            std::string detail;
            for (const auto& token : decode_semantic_tokens(result, content)) {
                detail += std::format("[{}:{} '{}' {} {}] ", token.line, token.startChar, token.text, token.type, lsp::dump(token.modifiers));
            }
            return { ok, detail.substr(0, std::min<std::size_t>(detail.size(), 200)) };
        }
        if (kind == "bundle") {
            // issue #23 fix plan F18: `mcppls.exportBundle` writes a zip within its size cap whose manifest is its contents,
            // digest for digest, and in which no file -- nor the report cxxModules/report answers -- names the home directory
            // the server runs with or the user it runs as (S3-5.5-3). The client's log it is sent carries the home too.
            const std::string home { base::normalize_path(options_.isolatedHome.empty() ? mcppls::platform::dirs::home_directory() : options_.isolatedHome) };
            std::vector<std::string> forbidden { home };
            forbidden.push_back(base::replace_all(home, "/", "\\"));
            for (const std::string_view name : { "USER", "USERNAME", "LOGNAME" }) {
                const auto user = mcppls::platform::env::get(name);
                if (!user || !mcppls::bundle::distinctive_name(*user)) continue;
                forbidden.push_back(*user);
                // Its 8.3 form (RUNNER~1 for runneradmin), which a Windows temporary directory is spelled with.
                if (user->size() > 8) forbidden.push_back(base::to_lower_ascii(user->substr(0, 6)) + "~");
            }
            // The report is redacted but keeps the project's own paths; only a bundle can be asked to hide them.
            const std::size_t forbiddenInReport { forbidden.size() };
            if (check.value("forbid-workspace", false)) forbidden.push_back(base::normalize_path(workspace_));
            const auto named = [&](std::string_view text, std::size_t count) -> std::string {
                const std::string lower { base::to_lower_ascii(text) };
                for (const auto& needle : std::span { forbidden }.first(count)) {
                    if (!needle.empty() && lower.contains(base::to_lower_ascii(needle))) return needle == home ? std::string { "the home directory" } : std::format("'{}'", needle);
                }
                return {};
            };
            std::string path;
            if (check.value("via", std::string { "command" }) == "cli") {
                // `mcppls report --bundle`, the way CI and a person without an editor export one.
                path = base::join_path(cacheDirectory_, std::format("{}.zip", check.value("id", std::string { "bundle" })));
                mcppls::platform::SpawnOptions spawn;
                spawn.program = options_.server;
                spawn.arguments = { "report", "--root", workspace_, "--settle", "30", "--bundle", path };
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
                if (result->timedOut || result->exitCode != 0) return { false, std::format("mcppls report --bundle: exit {}: {}", result->exitCode, (result->output + result->error).substr(0, 300)) };
            } else {
                Json arguments = check.value("arguments", Json::object());
                arguments["client"] = Json { { "name", "mcppls-conformance" }, { "log", std::format("started in {}\nworkspace {}\n", home, workspace_) } };
                const auto answer = client_.request("workspace/executeCommand", Json { { "command", "mcppls.exportBundle" }, { "arguments", Json::array({ arguments }) } },
                                                    timeout_);
                if (!answer || !answer->is_object() || !answer->contains("path")) return { false, "mcppls.exportBundle: no answer, or an error" };
                path = answer->value("path", std::string {});
            }
            auto archive = fs::read_file(path);
            if (archive && !options_.keepBundles.empty()) {
                (void)fs::create_directories(options_.keepBundles);
                (void)fs::write_file(base::join_path(options_.keepBundles, std::format("{}-{}.zip", base::file_name(options_.fixture), check.value("id", std::string { "bundle" }))), *archive);
            }
            fs::remove_all(path);
            if (!archive) return { false, std::format("no bundle at {}", path) };
            if (archive->size() > 25 * 1024 * 1024) return { false, std::format("the bundle is {} bytes, over its 25 MB cap", archive->size()) };
            auto files = mcppls::bundle::read_archive(*archive);
            if (!files) return { false, files.error() };
            if (!files->contains("manifest.json")) return { false, "no manifest.json" };
            const Json manifest = Json::parse(files->at("manifest.json"), nullptr, false);
            if (!manifest.is_object() || !manifest.contains("files")) return { false, "manifest.json is not a manifest" };
            if (manifest["files"].size() + 1 != files->size()) return { false, std::format("the manifest lists {} files, the bundle has {}", manifest["files"].size(), files->size() - 1) };
            for (const auto& file : manifest["files"]) {
                const std::string name { file.value("path", std::string {}) };
                const auto found = files->find(name);
                if (found == files->end()) return { false, std::format("{} is in the manifest, not in the bundle", name) };
                if (base::sha256_hex(found->second) != file.value("sha256", std::string {})) return { false, std::format("{} is not what the manifest's digest says", name) };
            }
            for (const auto& expected : check.value("expect-files", Json::array())) {
                if (!files->contains(expected.get<std::string>())) return { false, std::format("no {} in the bundle", expected.get<std::string>()) };
            }
            for (const auto& [name, content] : *files) {
                if (const auto what = named(content, forbidden.size()); !what.empty()) return { false, std::format("{} names {}", name, what) };
            }
            const auto report = client_.request("cxxModules/report", Json::object(), timeout_);
            if (!report) return { false, "cxxModules/report: no answer" };
            if (const auto what = named(lsp::dump(*report), forbiddenInReport); !what.empty()) return { false, std::format("cxxModules/report names {}", what) };
            return { true, std::format("{} files, {} bytes, redactions {}", files->size(), archive->size(), lsp::dump(manifest["redaction"]["rules"])) };
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
        if (kind == "clangd-lsp") return run_clangd_lsp(check, file);
        if (kind == "completion-baseline") return run_completion_baseline(check, file);
        if (kind == "latency") return run_latency(check);
        if (kind == "typing") return run_typing(check, file);
        if (kind == "edit-save") return run_edit_save(check);
        if (kind == "fault") return run_fault(check);
        if (kind == "timeline") return run_timeline(check);
        if (kind == "bmi-reuse") return run_bmi_reuse(check);
        if (kind == "resources") return run_resources(check);
        if (kind == "graph-edit") return run_graph_edit(check);
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
    if (!options.timingEvidenceFile.empty() && name != "timing") {
        say("conformance: --timing-evidence is only for the tiny timing fixture");
        return 2;
    }
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
    if (const auto prepend = scenario.find("server-path-prepend"); prepend != scenario.end() && prepend->is_string()) {
        options.pathPrepend = expand(prepend->get<std::string>(), expansion);
    }
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
    // Scenario tests count what the server built since it started: the module files and the log files there were before.
    std::map<std::string, std::string> pcmBefore;
    std::set<std::string> logsBefore;
    if (std::ranges::any_of(scenario.value("checks", Json::array()), [](const Json& check) { return check.value("kind", std::string {}) == "bmi-reuse"; })) {
        pcmBefore = all_module_files(cacheDirectory);
    }
    for (const auto& file : fs::list_directory(base::join_path(cacheDirectory, "logs"))) logsBefore.insert(file);
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
    // A scenario's own "initialization-options" object, merged on top of whatever the client
    // profile above set -- design doc 2026-09-25 K/§7's conformance cases ask for
    // {"semanticTokens": {"moduleType": true}} this way, without a client profile of their own.
    if (const auto extra = scenario.find("initialization-options"); extra != scenario.end() && extra->is_object()) {
        for (auto entry = extra->begin(); entry != extra->end(); ++entry) initializationOptions[entry.key()] = entry.value();
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
    // A scenario's own "client-info": the client this runner says it is (fix plan 2026-09-26 F9: what a
    // server tells VS Code differs from what it tells any other client).
    if (const auto info = scenario.find("client-info"); info != scenario.end() && info->is_object()) initializeParams["clientInfo"] = *info;
    auto initialized = client.request("initialize", initializeParams, std::chrono::seconds { 120 });
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

    // design doc 2026-09-25 K/§7: the legend this server just advertised, so a "semantic-tokens"
    // check can decode a result's type/modifier indices back into names.
    const Json semanticTokensLegend = lsp::find_path(*initialized, { "capabilities", "semanticTokensProvider", "legend" }) != nullptr
                                          ? (*initialized)["capabilities"]["semanticTokensProvider"]["legend"]
                                          : Json::object();
    Scenario runner { client, options, serverArguments, workspace, options.timeout, std::move(prepared), cacheDirectory, options.expectWarm,
                      std::move(moduleFilesBefore), semanticTokensLegend, initialized->value("capabilities", Json::object()) };
    runner.set_scenario_context(begin, initializeParams, std::move(pcmBefore), std::move(logsBefore));
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
        // `only-on`: the operating systems a check holds on ("linux", "macos", "windows"); a defect that shows
        // differently elsewhere (a crash instead of a spin) is checked by its own entry there.
        if (const auto onlyOn = check.find("only-on"); onlyOn != check.end() && onlyOn->is_array()) {
            const std::string_view here { mcppls::os::FAMILY == mcppls::os::Family::windows ? "windows"
                                          : mcppls::os::FAMILY == mcppls::os::Family::macos ? "macos" : "linux" };
            if (std::ranges::none_of(*onlyOn, [&](const Json& os) { return os.is_string() && os.get<std::string>() == here; })) {
                say("SKIP {} {} (only on {})", id, check.value("kind", std::string {}), lsp::dump(*onlyOn));
                continue;
            }
        }
        // `stage`: a check for one run of a fixture that has several (cold, warm, edits, faults, long): the others skip it.
        if (const auto stage = check.find("stage"); stage != check.end() && !options.stage.empty()) {
            const bool named { stage->is_array() ? std::ranges::any_of(*stage, [&](const Json& one) { return one == Json(options.stage); }) : *stage == Json(options.stage) };
            if (!named) continue;
        }
        if (const auto reason = client.unusable(); !reason.empty()) {
            if (!optional) ++failures;
            say("{} {} {} (not run) {}", optional ? "SKIP" : "FAIL", id, check.value("kind", std::string {}), reason);
            continue;
        }
        if (const auto notBefore = check.find("not-before-since-start"); notBefore != check.end() && notBefore->is_number()) runner.wait_until_since_start(notBefore->get<double>());
        const auto started = Clock::now();
        auto [ok, detail] = runner.run(check);
        const double seconds { std::chrono::duration<double>(Clock::now() - started).count() };
        // 0.0.7 scenario tests: two budgets any check can carry. "within-since-start": the check had what it waits for that many
        // seconds after initialize (a retried check ends when it first holds); "max-seconds": the check itself took no longer.
        if (ok) {
            if (const auto within = check.find("within-since-start"); within != check.end() && within->is_number() && runner.since_start() > within->get<double>()) {
                ok = false;
                detail = std::format("held {:.1f} s after initialize, over the budget {:.1f} s: {}", runner.since_start(), within->get<double>(), detail);
            }
            if (const auto longest = check.find("max-seconds"); longest != check.end() && longest->is_number() && seconds > longest->get<double>()) {
                ok = false;
                detail = std::format("took {:.1f} s, over the budget {:.1f} s: {}", seconds, longest->get<double>(), detail);
            }
        }
        if (!ok && !optional) ++failures;
        say("{} {} {} ({:.1f}s) {}", ok ? "PASS" : (optional ? "SKIP" : "FAIL"), id, check.value("kind", std::string {}), seconds, detail);
        Json entry { { "id", id }, { "kind", check.value("kind", std::string {}) }, { "ok", ok }, { "seconds", seconds },
                     { "since-start", std::chrono::duration<double>(Clock::now() - begin).count() }, { "detail", detail } };
        if (Json measure = runner.take_measure(); !measure.is_null()) entry["measure"] = std::move(measure);
        measured.push_back(std::move(entry));
    }
    // Snapshot after every measured check, while the server can still answer. Evidence is
    // optional and never changes a check result or its deadline; a missing report stays visible.
    auto measuredReady = client.firstReady;
    auto measuredDiagnostics = client.firstDiagnostics;
    double evidenceSeconds { 0 };
    if (!options.timingEvidenceFile.empty()) {
        const auto evidenceBegin = Clock::now();
        auto report = client.request("cxxModules/report", Json::object(), std::chrono::seconds { 2 });
        Json evidence { { "server-command", client.serverCommand }, { "workspace", workspace },
                        { "cache-directory", cacheDirectory }, { "report", nullptr },
                        { "collection-budget-seconds", 2 }, { "collection-excluded-from-timing", true } };
        if (report && report->dump(2).size() <= 1024 * 1024) evidence["report"] = std::move(*report);
        else evidence["report-unavailable"] = report ? "report exceeds 1 MiB evidence bound" : "no report within 2 seconds";
        evidence["collection-seconds"] = std::chrono::duration<double>(Clock::now() - evidenceBegin).count();
        if (auto written = fs::write_file(options.timingEvidenceFile, evidence.dump(2) + "\n"); !written)
            say("conformance: cannot write timing evidence {}", options.timingEvidenceFile);
        evidenceSeconds = std::chrono::duration<double>(Clock::now() - evidenceBegin).count();
    }
    runner.finish();
    client.stop();
    if (options.timingEvidenceFile.empty()) {
        measuredReady = client.firstReady;
        measuredDiagnostics = client.firstDiagnostics;
    }
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
    const double total { std::chrono::duration<double>(Clock::now() - begin).count() - evidenceSeconds };
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
        summary["ready"] = since(measuredReady);
        summary["first-diagnostics"] = since(measuredDiagnostics);
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

// compdb-lto-msvc (issue #23, fix plan F1): the compile_commands.json of a project built with LTO for the
// MSVC ABI, as CMake writes it for `-flto` with clang++ on Windows, with `compiler` (or clang++ on PATH)
// as the driver. Nothing is compiled: the fixture is about the commands the server gives clangd, whose
// module scan failed on `LTO requires -fuse-ld=lld` when they carried `-flto` and no `-c`. The driver
// raises that for the windows-msvc target on any host, so the fixture runs on Linux. `--no-default-config`
// makes the driver the one of the LLVM Windows installer, with no configuration file: an LLVM that
// carries one choosing lld (as mcpp's does) would not plan the link that fails.
int prepare_compdb_lto_msvc(const std::string& compiler) {
    const std::string root { fs::current_directory() };
    auto clangxx = on_path(compiler.empty() ? std::string { "clang++" } : compiler);
    if (!clangxx) {
        say("compdb-lto-msvc: {} is not on PATH", compiler);
        return 1;
    }
    Json database = Json::array();
    for (const std::string_view relative : { "src/answer.cppm", "src/main.cpp" }) {
        const std::string source { native(base::join_path(root, relative)) };
        database.push_back(Json { { "directory", native(root) }, { "file", source },
                                  { "arguments", Json::array({ *clangxx, "--no-default-config", "--target=x86_64-pc-windows-msvc", "-std=c++23",
                                                               "-flto", "-O2", "-c", source, "-o", source + ".obj" }) } });
    }
    if (auto written = fs::write_file(base::join_path(root, "compile_commands.json"), database.dump(2)); !written) {
        say("compdb-lto-msvc: {}", written.error().message);
        return 1;
    }
    return 0;
}

// Fix plan F6: a compile_commands.json whose commands carry an option value the compiler rejects, the kind of
// command #23's LTO one was: clangd's module scan fails on every unit, and the status says the command was rejected,
// with the driver's own words, instead of leaving the user to find "Scanning modules dependencies ... failed" in a log.
int prepare_compdb_rejected_command(const std::string& compiler) {
    const std::string root { fs::current_directory() };
    auto clangxx = on_path(compiler.empty() ? std::string { "clang++" } : compiler);
    if (!clangxx) {
        say("compdb-rejected-command: {} is not on PATH", compiler);
        return 1;
    }
    Json database = Json::array();
    for (const std::string_view relative : { "src/answer.cppm", "src/main.cpp" }) {
        const std::string source { native(base::join_path(root, relative)) };
        database.push_back(Json { { "directory", native(root) }, { "file", source },
                                  { "arguments", Json::array({ *clangxx, "-std=c++99999", "-c", source, "-o", source + ".o" }) } });
    }
    if (auto written = fs::write_file(base::join_path(root, "compile_commands.json"), database.dump(2)); !written) {
        say("compdb-rejected-command: {}", written.error().message);
        return 1;
    }
    return 0;
}

// 0.0.8 part 2 (X-7, X-5): a plain compile_commands.json over every C++ source under src/, each command carrying `marker` so
// that a check can tell which database the engine's commands came from: the user's stale file (xmake-late-config: the user ran
// `xmake project` once, before the project had a standard) or the file a tool is about to rewrite (compdb-midwrite).
int prepare_marked_compdb(std::string_view fixture, const std::string& compiler, const std::string& marker, const std::string& standard) {
    const std::string root { fs::current_directory() };
    auto driver = on_path(compiler);
    if (!driver) {
        say("{}: {} is not on PATH", fixture, compiler);
        return 1;
    }
    Json database = Json::array();
    static constexpr std::array<std::string_view, 2> EXTENSIONS { ".cpp", ".cppm" };
    for (const auto& source : fs::list_files(base::join_path(root, "src"), EXTENSIONS, {})) {
        Json arguments = Json::array({ *driver });
        if (!standard.empty()) arguments.push_back(standard);
        for (const auto& argument : { marker, std::string { "-c" }, native(source), std::string { "-o" }, native(source) + ".o" }) arguments.push_back(argument);
        database.push_back(Json { { "directory", native(root) }, { "file", native(source) }, { "arguments", std::move(arguments) } });
    }
    if (auto written = fs::write_file(base::join_path(root, "compile_commands.json"), database.dump(2)); !written) {
        say("{}: {}", fixture, written.error().message);
        return 1;
    }
    return 0;
}

// C++26 alignment (fix plan 2026-09-26 §9): a compile_commands.json whose units name two standards -- a module and an
// importer of std at C++23, an application at C++26 importing both. One std BMI cannot serve both standards.
int prepare_compdb_mixed_standards(const std::string& compiler) {
    const std::string root { fs::current_directory() };
    auto clangxx = on_path(compiler.empty() ? std::string { "clang++" } : compiler);
    if (!clangxx) {
        say("compdb-mixed-standards: {} is not on PATH", compiler);
        return 1;
    }
    Json database = Json::array();
    for (const auto& [relative, standard] : { std::pair { "src/core.cppm", "-std=c++23" }, std::pair { "src/legacy.cpp", "-std=c++23" },
                                              std::pair { "src/app.cpp", "-std=c++26" } }) {
        const std::string source { native(base::join_path(root, relative)) };
        database.push_back(Json { { "directory", native(root) }, { "file", source },
                                  { "arguments", Json::array({ *clangxx, "-stdlib=libc++", standard, "-c", source, "-o", source + ".o" }) } });
    }
    if (auto written = fs::write_file(base::join_path(root, "compile_commands.json"), database.dump(2)); !written) {
        say("compdb-mixed-standards: {}", written.error().message);
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

// Fix plan F3: a clangd that crashes the way clangd 23.1 did on Windows in issue #23 -- its crash context on standard
// error, naming a file that is not the one being edited, and then gone -- once, 25 seconds after it starts; every
// later start is the payload's real clangd. POSIX only: the stand-in is a shell script around the real one.
int prepare_clangd_crash_context(const std::string& payload) {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        say("clangd-crash-context: POSIX only");
        return 2;
    }
    const std::string workspace { fs::current_directory() };
    const std::string real { base::join_path(payload, "clangd/bin/clangd") };
    if (!fs::exists(real)) {
        say("clangd-crash-context: no clangd at {}", real);
        return 1;
    }
    const std::string directory { base::join_path(workspace, "stand-in") };
    (void)fs::create_directories(directory);
    const std::string crashed { base::join_path(directory, "crashed-once") };
    const std::string crasher { base::join_path(workspace, "src/crasher.cpp") };
    const std::string script { std::format(
        "#!/bin/sh\n"
        "real='{}'\n"
        "case \"$1\" in --version|--help) exec \"$real\" \"$@\" ;; esac\n"
        "[ -e '{}' ] && exec \"$real\" \"$@\"\n"
        ": > '{}'\n"
        // A background job of sh reads /dev/null: the editor's input goes to clangd through a descriptor kept first.
        "exec 3<&0\n"
        "\"$real\" \"$@\" <&3 3<&- &\n"
        "child=$!\n"
        // The wrapper must look busy for the stuck watch: it samples this process's own CPU, and an idle
        // shell around a working clangd reads as a dead engine (the first answer then lost the 3s+5s race
        // on the slowest Intel macOS runners, which killed the stand-in before its designed crash).
        "end=$(( $(date +%s) + 25 ))\n"
        "while [ $(date +%s) -lt $end ]; do i=0; while [ $i -lt 5000 ]; do i=$((i+1)); done; done\n"
        "echo 'PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/ and include the crash backtrace.' >&2\n"
        "echo 'Signalled during AST worker action: Build AST' >&2\n"
        "echo '  Filename: {}' >&2\n"
        "echo '  Directory: {}' >&2\n"
        "echo '  Command Line: clang++ -std=c++23 -c -- {}' >&2\n"
        "echo '  Version: 1' >&2\n"
        "kill -KILL $child\n"
        "exit 139\n",
        real, crashed, crashed, crasher, workspace, crasher) };
    const std::string clangd { base::join_path(directory, "clangd") };
    if (auto written = fs::write_file(clangd, script); !written) {
        say("clangd-crash-context: {}", written.error().message);
        return 1;
    }
    if (auto marked = fs::make_executable(std::vector<std::string> { clangd }); !marked) {
        say("clangd-crash-context: {}", marked.error().message);
        return 1;
    }
    return 0;
}

// 0.0.8 plan R-4: a producer that answers late. A shell script around the mock mcpp that waits `seconds` before
// `emit build-database` and lets every other command through at once, so the server plans with what it scanned itself
// meanwhile and then receives the build tool's model. POSIX only.
int prepare_delayed_producer(const std::string& seconds) {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        say("delayed-producer: POSIX only");
        return 2;
    }
    const std::string self { absolute(mcppls::platform::env::arguments().front()) };
    const std::string mock { base::join_path(base::parent_path(self), "mcppls-mock-mcpp") + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
    if (!fs::exists(mock)) {
        say("delayed-producer: {} is not built (needs mcppls-mock-mcpp beside mcppls-conformance)", mock);
        return 1;
    }
    const std::string directory { base::join_path(fs::current_directory(), "stand-in") };
    (void)fs::create_directories(directory);
    const std::string script { std::format(
        "#!/bin/sh\n"
        "[ \"$1\" = emit ] && sleep {}\n"
        "exec '{}' \"$@\"\n",
        seconds.empty() ? std::string { "10" } : seconds, mock) };
    const std::string path { base::join_path(directory, "mcpp") };
    if (auto written = fs::write_file(path, script); !written) {
        say("delayed-producer: {}", written.error().message);
        return 1;
    }
    if (auto marked = fs::make_executable(std::vector<std::string> { path }); !marked) {
        say("delayed-producer: {}", marked.error().message);
        return 1;
    }
    return 0;
}

// M-2, M-3 (plan 0.0.9): a header that costs as much to preprocess as a big library's, in src/heavy/ (`#include "heavy/all.hpp"`
// from any file in src/): <argument> (default 150) headers of 60 class templates, variable templates, functions and macros
// each, which all.hpp includes after some of the standard library's own. Nothing in them needs a network or a licence, and a
// file that includes them is parsed, and its modules scanned (WA-CLANGD-009), as the file of a project that uses such a library.
int prepare_heavy_headers(const std::string& argument) {
    const int count { argument.empty() ? 150 : std::max(1, std::atoi(argument.c_str())) };
    const std::string directory { base::join_path(fs::current_directory(), "src/heavy") };
    (void)fs::create_directories(directory);
    std::string all { "#pragma once\n#include <algorithm>\n#include <map>\n#include <ranges>\n#include <regex>\n#include <string>\n#include <vector>\n" };
    for (int unit { 0 }; unit < count; ++unit) {
        const std::string name { std::format("h{:03}", unit) };
        std::string text { std::format("#pragma once\n#include <cstddef>\n#include <type_traits>\nnamespace heavy::n{:03} {{\n", unit) };
        for (int member { 0 }; member < 60; ++member) {
            text += std::format("template <typename T, std::size_t N = {1}> struct Box{0} {{ T items[N]; constexpr std::size_t size() const {{ return N; }} }};\n", member, member + 1);
            text += std::format("template <typename T> constexpr bool is_box{0}_v = std::is_class_v<Box{0}<T>>;\n", member);
            text += std::format("inline int fn{0}(int x) {{ return x * {0} + {1}; }}\n", member, unit);
            text += std::format("#define HEAVY_{1:03}_{0}(a, b) ((a) + (b) * {0})\n", member, unit);
        }
        text += "}\n";
        if (auto written = fs::write_file(base::join_path(directory, name + ".hpp"), text); !written) {
            say("prepare: {}", written.error().message);
            return 1;
        }
        all += std::format("#include \"heavy/{}.hpp\"\n", name);
    }
    if (auto written = fs::write_file(base::join_path(directory, "all.hpp"), all); !written) {
        say("prepare: {}", written.error().message);
        return 1;
    }
    return 0;
}

int prepare(const std::string& kind, const std::string& argument) {
    if (kind == "heavy-headers") return prepare_heavy_headers(argument);
    if (kind == "s1-two-sets") return prepare_s1_two_sets(argument);
    if (kind == "payload-corrupt") return prepare_payload_corrupt(argument);
    if (kind == "producer-candidate") return prepare_producer_candidate(argument);
    if (kind == "delayed-producer") return prepare_delayed_producer(argument);
    if (kind == "failure-at-base") return prepare_failure_at_base(argument);
    if (kind == "compdb-clang-cl-std") return prepare_compdb_msvc_std(true);
    if (kind == "compdb-clangxx-msvc-std") return prepare_compdb_msvc_std(false);
    if (kind == "generated-module-old-mcpp") return prepare_generated_module_compdb(argument);
    if (kind == "clangd-cannot-load") return prepare_clangd_cannot_load();
    if (kind == "clangd-crash-context") return prepare_clangd_crash_context(argument);
    if (kind == "compdb-rejected-command") return prepare_compdb_rejected_command(argument);
    if (kind == "compdb-mixed-standards") return prepare_compdb_mixed_standards(argument);
    if (kind == "xmake-stale-compdb") return prepare_marked_compdb(kind, argument.empty() ? std::string { "g++" } : argument, "-DSTALE_COMPDB", "-std=c++17");
    if (kind == "compdb-midwrite") return prepare_marked_compdb(kind, argument.empty() ? std::string { "clang++" } : argument, "-DVERSION_ONE", "-std=c++23");
    if (kind == "compdb-lto-msvc") return prepare_compdb_lto_msvc(argument);
    say("prepare: unknown fixture kind {} (heavy-headers, s1-two-sets, payload-corrupt, producer-candidate, delayed-producer, failure-at-base, compdb-clang-cl-std, compdb-clangxx-msvc-std, generated-module-old-mcpp, clangd-cannot-load, compdb-lto-msvc, clangd-crash-context, compdb-rejected-command, compdb-mixed-standards, xmake-stale-compdb, compdb-midwrite)", kind);
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
    (void)runCommand.option("timing-evidence").takes_value().help("Bounded report after timing fixture checks; excluded from timing budgets");
    (void)runCommand.option("expect-warm").help("module-cache-reused checks fail unless an earlier run left module files in --cache-dir");
    (void)runCommand.option("navigation-budget").takes_value().help("Seconds the first navigation may take from initialize; more fails the run");
    (void)runCommand.option("no-dynamic-watch").help("Do not advertise didChangeWatchedFiles.dynamicRegistration, exercising the polling fallback");
    (void)runCommand.option("plain-client").help("Alias for --client plain");
    (void)runCommand.option("client").takes_value().help("The capabilities a real editor sends: vscode, neovim, zed or plain (default: this runner's own, the full experimental.cxxModules block)");
    (void)runCommand.option("stress-seed").takes_value().help("Overrides every stress check's own \"seed\" (mcppls-devtools stress --seed)");
    (void)runCommand.option("stage").takes_value().help("Run only the checks of this stage (a check's \"stage\"), and those with none");
    (void)runCommand.option("keep-bundles").takes_value().help("Directory a copy of every diagnostic bundle a bundle check exported is left in");
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
        options.timingEvidenceFile = args.value("timing-evidence") ? absolute(*args.value("timing-evidence")) : std::string {};
        options.expectWarm = args.is_flag_set("expect-warm");
        options.noDynamicWatch = args.is_flag_set("no-dynamic-watch");
        options.plainClient = args.is_flag_set("plain-client");
        options.stage = args.value("stage").value_or("");
        options.keepBundles = args.value("keep-bundles") ? absolute(*args.value("keep-bundles")) : std::string {};
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
