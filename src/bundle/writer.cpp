module mcppls.bundle.writer;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.base.text;
import mcppls.base.version;
import mcppls.platform.dirs;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.bundle.identity;
import mcppls.bundle.redact;
import mcppls.bundle.zip;

namespace mcppls::bundle {

namespace {

using Json = nlohmann::json;
namespace log = base::log;

// What goes first when the bundle would be larger than its cap: the report and the environment
// always, then the incidents, the engine, the client's log, the server's logs newest first, dumps last.
enum class Kind { report, environment, incident, engine, client_log, log, dump };

struct Candidate {
    std::string name;   // in the archive
    std::string content;
    Kind kind { Kind::report };
    bool text { true };   // redacted and checked; a dump is neither
    bool truncated { false };
    std::string note;
};

constexpr std::size_t KIB { 1024 };
constexpr std::size_t MIB { 1024 * KIB };
constexpr std::size_t LOG_FILE_CAP { 2 * MIB };
constexpr std::size_t LOG_HEAD { 256 * KIB };
constexpr std::size_t INCIDENT_FILE_CAP { 4 * MIB };
constexpr std::size_t DATABASE_CAP { 8 * MIB };
constexpr std::size_t PROBES_CAP { 1 * MIB };
constexpr std::size_t CLIENT_LOG_CAP { 2 * MIB };
constexpr std::uint64_t MANIFEST_RESERVE { 256 * KIB };
constexpr std::size_t LOG_SESSIONS { 3 };        // the last sessions, whenever they were
constexpr std::size_t LOG_SESSIONS_MAX { 6 };    // and any other of the last day, up to this many in all

std::string dump(const Json& value) { return value.dump(2, ' ', false, Json::error_handler_t::replace); }

std::string utc_now(std::string_view format) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return std::vformat(format, std::make_format_args(now));
}

bool binary(std::string_view content) { return content.substr(0, std::min<std::size_t>(content.size(), 8 * KIB)).contains('\0'); }

// The first and the last of a long text, with a line that says how much was left out.
std::string head_and_tail(std::string content, std::size_t cap, bool& truncated) {
    if (content.size() <= cap) return content;
    truncated = true;
    const std::size_t tail { cap - LOG_HEAD };
    std::string kept { content.substr(0, LOG_HEAD) };
    kept += std::format("\n[... mcppls left out {} bytes here to keep the bundle small ...]\n", content.size() - LOG_HEAD - tail);
    kept += content.substr(content.size() - tail);
    return kept;
}

std::optional<std::string> read_text(std::string_view path) {
    auto content = platform::fs::read_file(path);
    if (!content) return std::nullopt;
    return std::move(*content);
}

// A system's own answer about itself (a version, a code page), bounded and in this process's own
// environment: never the login shell, never the network.
std::optional<std::string> system_answer(std::string_view program, std::vector<std::string> arguments) {
    const auto executable = platform::env::find_executable(program);
    if (!executable) return std::nullopt;
    auto result = platform::toolrun::run(platform::toolrun::Request {
        .program = *executable,
        .arguments = std::move(arguments),
        .workDirectory = platform::dirs::temp_directory(),
        .purpose = "bundle",
        .bounds = platform::RunBounds { .hard = std::chrono::seconds { 5 } },
        .environment = platform::env::variables(),
    });
    if (!result || result->exitCode != 0 || result->timedOut) return std::nullopt;
    return std::move(result->output);
}

// "Key:\tvalue" lines, the way sw_vers answers.
std::string field(std::string_view text, std::string_view key) {
    for (const auto line : base::split_lines(text)) {
        const auto trimmed = base::trim(line);
        if (trimmed.starts_with(key)) {
            auto rest = trimmed.substr(key.size());
            if (!rest.empty() && rest.front() == ':') rest.remove_prefix(1);
            return std::string { base::trim(rest) };
        }
    }
    return {};
}

std::string unquoted(std::string_view value) {
    value = base::trim(value);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) value = value.substr(1, value.size() - 2);
    return std::string { value };
}

Json operating_system() {
    Json os { { "platform", std::string { mcppls::os::PLATFORM } }, { "family", std::string { mcppls::os::FAMILY_NAME } } };
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::linux) {
        if (auto release = read_text("/proc/sys/kernel/osrelease")) os["kernel"] = std::string { base::trim(*release) };
        if (auto release = read_text("/etc/os-release")) {
            for (const auto line : base::split_lines(*release)) {
                if (line.starts_with("PRETTY_NAME=")) os["distribution"] = unquoted(line.substr(12));
            }
        }
    } else if constexpr (mcppls::os::FAMILY == mcppls::os::Family::macos) {
        if (auto versions = system_answer("sw_vers", {})) {
            os["name"] = field(*versions, "ProductName");
            os["version"] = field(*versions, "ProductVersion");
            os["build"] = field(*versions, "BuildVersion");
        }
    } else {
        if (auto version = system_answer("cmd", { "/d", "/c", "ver" })) os["version"] = std::string { base::trim(*version) };
    }
    return os;
}

Json memory() {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::linux) {
        if (auto info = read_text("/proc/meminfo")) {
            for (const auto line : base::split_lines(*info)) {
                if (!line.starts_with("MemTotal:")) continue;
                std::uint64_t kib { 0 };
                const auto digits = base::trim(line.substr(9));
                (void)std::from_chars(digits.data(), digits.data() + digits.size(), kib);
                return Json { { "totalBytes", kib * 1024 } };
            }
        }
    } else if constexpr (mcppls::os::FAMILY == mcppls::os::Family::macos) {
        if (auto bytes = system_answer("sysctl", { "-n", "hw.memsize" })) {
            std::uint64_t total { 0 };
            const auto digits = base::trim(*bytes);
            (void)std::from_chars(digits.data(), digits.data() + digits.size(), total);
            return Json { { "totalBytes", total } };
        }
    }
    return nullptr;
}

Json locale() {
    Json locale = Json::object();
    for (const std::string_view name : { "LANG", "LC_ALL", "LC_CTYPE" }) {
        if (auto value = platform::env::get(name)) locale[std::string { name }] = *value;
    }
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        // The ANSI code page is what a narrow string from the system is in (issue #23 had a GBK machine).
        if (auto answer = system_answer("reg", { "query", "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage", "/v", "ACP" })) {
            for (const auto line : base::split_lines(*answer)) {
                const auto words = base::split(base::trim(line), ' ');
                std::vector<std::string_view> parts;
                for (const auto word : words) {
                    if (!base::trim(word).empty()) parts.push_back(base::trim(word));
                }
                if (parts.size() >= 3 && parts.front() == "ACP") locale["ansiCodePage"] = std::string { parts.back() };
            }
        }
    }
    return locale;
}

// Only these: a variable can carry a credential, and nothing else here needs one.
bool whitelisted_variable(std::string_view name) {
    const std::string upper { [&] {
        std::string out { name };
        for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }() };
    return upper.starts_with("MCPP_") || upper.starts_with("XLINGS_") || upper.starts_with("LC_") || upper == "LANG" || upper == "PATH";
}

Json environment_variables() {
    Json variables = Json::object();
    for (const auto& entry : platform::env::variables()) {
        const std::size_t equals { entry.find('=', 1) };
        if (equals == std::string::npos) continue;
        const std::string name { entry.substr(0, equals) };
        if (whitelisted_variable(name)) variables[name] = entry.substr(equals + 1);
    }
    return variables;
}

Json environment(const BundleInput& input) {
    const Json& report { input.report };
    Json versions { { "mcppls", std::string { base::VERSION } } };
    if (const auto payload = report.find("payload"); payload != report.end() && payload->is_object()) {
        versions["clangd"] = payload->value("clangdVersion", std::string {});
    }
    Json producers = Json::array();
    if (const auto roots = report.find("roots"); roots != report.end() && roots->is_array()) {
        for (const auto& root : *roots) {
            const Json* project { root.contains("project") && root["project"].is_object() ? &root["project"] : nullptr };
            if (project == nullptr) continue;
            producers.push_back(Json { { "producer", project->value("producer", std::string {}) }, { "version", project->value("producerVersion", std::string {}) } });
        }
    }
    versions["producers"] = std::move(producers);
    Json payload = nullptr;
    const std::string payloadDirectory { report.contains("payload") && report["payload"].is_object() ? report["payload"].value("directory", std::string {}) : std::string {} };
    if (!payloadDirectory.empty()) {
        if (auto text = read_text(base::join_path(payloadDirectory, "payload.json"))) {
            payload = Json::parse(*text, nullptr, false);
            if (payload.is_discarded()) payload = nullptr;
        }
    }
    Json probes = nullptr;
    if (auto text = read_text(base::join_path(platform::dirs::cache_directory(), "toolchains/probe.json")); text && text->size() <= PROBES_CAP) {
        probes = Json::parse(*text, nullptr, false);
        if (probes.is_discarded()) probes = nullptr;
    }
    return Json {
        { "generatedAt", utc_now("{:%FT%TZ}") },
        { "os", operating_system() },
        { "cpu", Json { { "logicalProcessors", std::thread::hardware_concurrency() } } },
        { "memory", memory() },
        { "locale", locale() },
        { "editor", report.value("client", Json(nullptr)) },
        { "client", input.client },
        { "initializationOptions", input.initializationOptions },
        { "server", report.value("server", Json(nullptr)) },
        { "payload", payload },
        { "versions", std::move(versions) },
        { "toolchainProbes", std::move(probes) },
        { "environmentVariables", environment_variables() },
    };
}

// server-<yyyymmdd>-<hhmmss>-<tag>.log and its rotations (.log.1, .log.2): the time it started.
std::optional<std::chrono::sys_seconds> session_start(std::string_view name) {
    static constexpr std::string_view PREFIX { "server-" };
    if (!name.starts_with(PREFIX) || name.size() < PREFIX.size() + 15) return std::nullopt;
    const std::string_view stamp { name.substr(PREFIX.size(), 15) };
    int year { 0 }, month { 0 }, day { 0 }, hour { 0 }, minute { 0 }, second { 0 };
    const auto number = [&](std::size_t at, std::size_t length, int& out) {
        return std::from_chars(stamp.data() + at, stamp.data() + at + length, out).ec == std::errc {};
    };
    if (stamp[8] != '-' || !number(0, 4, year) || !number(4, 2, month) || !number(6, 2, day) || !number(9, 2, hour) || !number(11, 2, minute)
        || !number(13, 2, second)) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date { std::chrono::year { year }, std::chrono::month { static_cast<unsigned>(month) },
                                             std::chrono::day { static_cast<unsigned>(day) } };
    if (!date.ok()) return std::nullopt;
    return std::chrono::sys_days { date } + std::chrono::hours { hour } + std::chrono::minutes { minute } + std::chrono::seconds { second };
}

// The server's logs worth reading: the last LOG_SESSIONS sessions, and any other of the last day,
// up to LOG_SESSIONS_MAX in all, each with its rotations, newest first.
std::vector<std::string> recent_logs(std::string_view directory, std::string_view currentLog) {
    std::map<std::string, std::vector<std::string>, std::greater<>> sessions;   // "server-<time>-<tag>" -> its files
    for (auto& path : platform::fs::list_directory(directory)) {
        const std::string_view name { base::file_name(path) };
        const std::size_t extension { name.find(".log") };
        if (!name.starts_with("server-") || extension == std::string_view::npos) continue;
        sessions[std::string { name.substr(0, extension) }].push_back(std::move(path));
    }
    const auto now = std::chrono::system_clock::now();
    std::vector<std::string> logs;
    std::size_t taken { 0 };
    for (auto& [session, files] : sessions) {
        const auto started = session_start(session);
        const bool recent { started && now - *started < std::chrono::hours { 24 } };
        if (taken >= LOG_SESSIONS_MAX || (taken >= LOG_SESSIONS && !recent)) break;
        ++taken;
        std::ranges::sort(files);   // x.log before x.log.1: newest content first
        for (auto& file : files) logs.push_back(std::move(file));
    }
    // A log file somewhere else (a server started with its own) is this session's all the same.
    if (!currentLog.empty() && std::ranges::none_of(logs, [&](const std::string& log) { return base::same_path(log, currentLog); })) {
        logs.insert(logs.begin(), std::string { currentLog });
    }
    return logs;
}

void strip_excerpts(Json& value) {
    if (value.is_object()) {
        for (const std::string_view key : { "excerpts", "excerpt", "sourceExcerpts" }) value.erase(std::string { key });
        for (auto& item : value) strip_excerpts(item);
    } else if (value.is_array()) {
        for (auto& item : value) strip_excerpts(item);
    }
}

// Each root's cache directory, as its report names it.
std::vector<std::string> root_caches(const Json& report) {
    std::vector<std::string> caches;
    if (const auto roots = report.find("roots"); roots != report.end() && roots->is_array()) {
        for (const auto& root : *roots) caches.push_back(root.is_object() ? root.value("cacheDirectory", std::string {}) : std::string {});
    }
    return caches;
}

std::vector<std::string> root_paths(const Json& report) {
    std::vector<std::string> paths;
    if (const auto roots = report.find("roots"); roots != report.end() && roots->is_array()) {
        for (const auto& root : *roots) {
            if (root.is_object() && !root.value("root", std::string {}).empty()) paths.push_back(root.value("root", std::string {}));
        }
    }
    return paths;
}

void add_incidents(std::vector<Candidate>& candidates, std::size_t rootIndex, std::string_view cache, const BundleOptions& options) {
    const std::string directory { base::join_path(cache, "incidents") };
    if (!platform::fs::is_directory(directory)) return;
    auto incidents = platform::fs::list_directory(directory);
    std::ranges::sort(incidents, std::greater<> {});   // names start with their UTC time: newest first
    for (const auto& incident : incidents) {
        const std::vector<std::string> files { platform::fs::is_directory(incident) ? platform::fs::list_files(incident, {}, {})
                                                                                    : std::vector<std::string> { incident } };
        for (const auto& file : files) {
            auto content = read_text(file);
            if (!content) continue;
            const auto relative = base::relative_path(file, directory);
            const std::string inside { std::format("incidents/root-{}/{}", rootIndex, relative.value_or(std::string { base::file_name(file) })) };
            const bool dumpFile { base::extension(file) == ".dmp" };
            if (dumpFile || binary(*content)) {
                if (dumpFile && options.includeDumps) {
                    candidates.push_back(Candidate { std::format("dumps/root-{}/{}", rootIndex, relative.value_or(std::string { base::file_name(file) })),
                                                     std::move(*content), Kind::dump, false, false, "not redacted: a dump holds memory" });
                }
                continue;
            }
            Candidate candidate { inside, {}, Kind::incident };
            if (!options.sourceExcerpts && base::extension(file) == ".json") {
                Json parsed = Json::parse(*content, nullptr, false);
                if (!parsed.is_discarded()) {
                    strip_excerpts(parsed);
                    *content = dump(parsed);
                    candidate.note = "source excerpts left out";
                }
            }
            candidate.content = head_and_tail(std::move(*content), INCIDENT_FILE_CAP, candidate.truncated);
            candidates.push_back(std::move(candidate));
        }
    }
}

void add_engine(std::vector<Candidate>& candidates, std::size_t rootIndex, std::string_view cache, const Json& plan) {
    if (!plan.is_null()) candidates.push_back(Candidate { std::format("engine/root-{}/plan.json", rootIndex), dump(plan), Kind::engine });
    if (cache.empty()) return;
    auto database = read_text(base::join_path(cache, "contexts/default/cdb/compile_commands.json"));
    if (!database) return;
    Candidate candidate { std::format("engine/root-{}/compile_commands.json", rootIndex), {}, Kind::engine };
    if (database->size() > DATABASE_CAP) {
        // Whole entries only, so what is kept is still a database.
        Json entries = Json::parse(*database, nullptr, false);
        if (entries.is_array()) {
            Json kept = Json::array();
            std::size_t bytes { 2 };
            for (auto& entry : entries) {
                const std::size_t size { entry.dump().size() + 2 };
                if (bytes + size > DATABASE_CAP) break;
                bytes += size;
                kept.push_back(std::move(entry));
            }
            candidate.note = std::format("{} of {} entries", kept.size(), entries.size());
            candidate.truncated = true;
            *database = dump(kept);
        } else {
            candidate.content = head_and_tail(std::move(*database), DATABASE_CAP, candidate.truncated);
            candidates.push_back(std::move(candidate));
            return;
        }
    }
    candidate.content = std::move(*database);
    candidates.push_back(std::move(candidate));
}

void add_dumps(std::vector<Candidate>& candidates, std::string_view directory, std::string_view prefix) {
    if (!platform::fs::is_directory(directory)) return;
    for (const auto& file : platform::fs::list_files(directory, std::array<std::string_view, 1> { ".dmp" }, {})) {
        auto content = read_text(file);
        if (!content) continue;
        candidates.push_back(Candidate { std::format("dumps/{}/{}", prefix, base::file_name(file)), std::move(*content), Kind::dump, false, false,
                                         "not redacted: a dump holds memory" });
    }
}

std::string default_output() {
    return base::join_path(platform::dirs::cache_directory(), std::format("bundles/mcppls-bundle-{}.zip", utc_now("{:%Y%m%dT%H%M%S}Z")));
}

void keep_newest_bundles(std::string_view directory) {
    std::vector<std::string> bundles;
    for (auto& path : platform::fs::list_directory(directory)) {
        const std::string_view name { base::file_name(path) };
        if (name.starts_with("mcppls-bundle-") && name.ends_with(".zip")) bundles.push_back(std::move(path));
    }
    std::ranges::sort(bundles, std::greater<> {});
    for (std::size_t i { BUNDLES_KEPT }; i < bundles.size(); ++i) platform::fs::remove_all(bundles[i]);
}

std::string_view kind_name(Kind kind) {
    switch (kind) {
    case Kind::report: return "report";
    case Kind::environment: return "environment";
    case Kind::incident: return "incident";
    case Kind::engine: return "engine";
    case Kind::client_log: return "client-log";
    case Kind::log: return "log";
    case Kind::dump: return "dump";
    }
    return "?";
}

} // namespace

Json redact_report(const Json& report) {
    Redactor redactor { current_identity() };
    return redactor.redact_json(report);
}

std::expected<BundleResult, BundleFailure> write_bundle(const BundleInput& input, const BundleOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    std::vector<Candidate> candidates;
    candidates.push_back(Candidate { "report.json", dump(input.report), Kind::report });
    candidates.push_back(Candidate { "environment.json", dump(environment(input)), Kind::environment });
    const auto caches = root_caches(input.report);
    const Json& roots { input.report.contains("roots") && input.report["roots"].is_array() ? input.report["roots"] : Json::array() };
    for (std::size_t i { 0 }; i < caches.size(); ++i) {
        if (!caches[i].empty()) add_incidents(candidates, i + 1, caches[i], options);
        add_engine(candidates, i + 1, caches[i], i < roots.size() && roots[i].is_object() ? roots[i].value("plan", Json(nullptr)) : Json(nullptr));
        if (options.includeDumps && !caches[i].empty()) add_dumps(candidates, base::join_path(caches[i], "dumps"), std::format("root-{}", i + 1));
    }
    if (!input.clientLog.empty()) {
        Candidate candidate { "logs/client.log", {}, Kind::client_log };
        candidate.content = head_and_tail(input.clientLog, CLIENT_LOG_CAP, candidate.truncated);
        candidates.push_back(std::move(candidate));
    }
    const std::string logDirectory { base::join_path(platform::dirs::cache_directory(), "logs") };
    const std::string currentLog { input.report.contains("server") && input.report["server"].is_object()
                                       ? input.report["server"].value("logFile", std::string {}) : std::string {} };
    for (const auto& file : recent_logs(logDirectory, currentLog)) {
        auto content = read_text(file);
        if (!content) continue;
        Candidate candidate { std::format("logs/{}", base::file_name(file)), {}, Kind::log };
        candidate.content = head_and_tail(std::move(*content), LOG_FILE_CAP, candidate.truncated);
        candidates.push_back(std::move(candidate));
    }
    if (options.includeDumps) add_dumps(candidates, base::join_path(platform::dirs::cache_directory(), "dumps"), "server");

    // One redactor for the whole bundle, so an original has one placeholder in every file.
    std::optional<Redactor> redactor;
    if (options.redact) {
        Identity identity { current_identity() };
        if (options.hideProjectPaths) identity = hiding_workspaces(std::move(identity), root_paths(input.report));
        redactor.emplace(std::move(identity));
    }
    BundleFailure failure;
    for (auto& candidate : candidates) {
        if (!candidate.text || !redactor) continue;
        candidate.content = redactor->redact(candidate.content);
        for (const auto& residue : redactor->residue(candidate.content)) {
            failure.residue.push_back(Json { { "file", candidate.name }, { "rule", residue.rule }, { "offset", residue.offset } });
        }
    }
    if (!failure.residue.empty()) {
        const Json& first { failure.residue.front() };
        failure.message = std::format("the diagnostic bundle was not written: {} still has what rule {} replaces (at byte {}){}. "
                                      "Nothing was written; try again with project paths hidden, or report this",
                                      first.value("file", std::string {}), first.value("rule", std::string {}), first.value("offset", std::size_t { 0 }),
                                      failure.residue.size() > 1 ? std::format(", and {} more", failure.residue.size() - 1) : std::string {});
        log::warning("{}", failure.message);
        return std::unexpected { std::move(failure) };
    }

    // Compressed one by one, then kept in order of what matters most until the cap.
    std::ranges::stable_sort(candidates, {}, &Candidate::kind);
    std::vector<ZipEntry> entries;
    Json files = Json::array();
    Json omitted = Json::array();
    std::uint64_t total { MANIFEST_RESERVE + 22 };
    for (auto& candidate : candidates) {
        ZipEntry entry { make_entry(candidate.name, candidate.content) };
        const bool always { candidate.kind == Kind::report || candidate.kind == Kind::environment };
        if (!always && total + archive_size(entry) > options.sizeCap) {
            omitted.push_back(Json { { "path", candidate.name }, { "bytes", candidate.content.size() }, { "why", "the bundle's size cap" } });
            continue;
        }
        total += archive_size(entry);
        Json file { { "path", candidate.name }, { "kind", std::string { kind_name(candidate.kind) } }, { "bytes", candidate.content.size() },
                    { "sha256", base::sha256_hex(candidate.content) }, { "redacted", candidate.text && redactor.has_value() } };
        if (candidate.truncated) file["truncated"] = true;
        if (!candidate.note.empty()) file["note"] = candidate.note;
        files.push_back(std::move(file));
        entries.push_back(std::move(entry));
    }
    BundleResult result;
    if (redactor) {
        for (const auto& [rule, count] : redactor->hits()) result.redactions[rule] = count;
    }
    Json manifest {
        { "format", FORMAT_VERSION },
        { "generatedAt", utc_now("{:%FT%TZ}") },
        { "generator", Json { { "name", "mcppls" }, { "version", std::string { base::VERSION } }, { "platform", std::string { mcppls::os::PLATFORM } } } },
        { "options", Json { { "redacted", options.redact }, { "hideProjectPaths", options.hideProjectPaths }, { "sourceExcerpts", options.sourceExcerpts },
                             { "includeDumps", options.includeDumps }, { "sizeCapBytes", options.sizeCap } } },
        { "redaction", options.redact ? Json { { "rules", result.redactions }, { "residueCheck", "passed" } }
                                      : Json { { "rules", Json::object() }, { "residueCheck", "off: --no-redact" } } },
        { "files", std::move(files) },
        { "omitted", std::move(omitted) },
    };
    entries.insert(entries.begin(), make_entry("manifest.json", dump(manifest)));
    const std::string archive { zip_archive(entries, std::chrono::system_clock::now()) };
    const bool defaultPlace { options.output.empty() };
    result.path = defaultPlace ? default_output() : base::normalize_path(options.output);
    (void)platform::fs::create_directories(base::parent_path(result.path));
    if (auto written = platform::fs::write_file_atomic(result.path, archive); !written) {
        return std::unexpected { BundleFailure { std::format("the diagnostic bundle could not be written to {}: {}", result.path, written.error().message) } };
    }
    if (defaultPlace) keep_newest_bundles(base::parent_path(result.path));
    result.bytes = archive.size();
    result.manifest = std::move(manifest);
    log::info("diagnostic bundle written in {} ms: {} ({} bytes, {} files)", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(),
              result.path, result.bytes, entries.size());
    return result;
}

} // namespace mcppls::bundle
