module mcppls.orchestrator.report;

import std;
import nlohmann.json;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.version;
import mcppls.engine.payload;
import mcpplibs.os;
import mcppls.toolrun;
import mcppls.base.home;
import mcppls.base.payload;

namespace os = mcpplibs::os;

namespace mcppls::orchestrator {

using Json = nlohmann::json;
namespace log = base::log;

namespace {

std::string_view level_name(log::Level level) {
    switch (level) {
    case log::Level::debug: return "debug";
    case log::Level::info: return "info";
    case log::Level::warning: return "warning";
    case log::Level::error: return "error";
    case log::Level::off: return "off";
    }
    return "?";
}

std::string utc_now(std::string_view format) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return std::vformat(format, std::make_format_args(now));
}

} // namespace

Json make_report(Json roots, Json client, std::string_view engine, const engine::PayloadPaths& payload, bool payloadCorrupt,
                 std::chrono::steady_clock::duration uptime, Json settings) {
    return Json {
        { "generatedAt", utc_now("{:%FT%TZ}") },
        { "server", Json { { "name", "mcppls" }, { "version", std::string { base::VERSION } }, { "platform", std::string { base::payload_platform() } },
                           { "uptimeSeconds", std::chrono::duration_cast<std::chrono::seconds>(uptime).count() },
                           { "logLevel", std::string { level_name(log::level()) } }, { "logFile", log::file_path() },
                           // X-5 (plan 2026-09-30): the server runs under PRoot (termux), where programs are started otherwise.
                           { "sandbox", toolrun::sandbox().empty() ? Json(nullptr) : Json(toolrun::sandbox()) } } },
        { "client", std::move(client) },
        { "engine", std::string { engine } },
        { "payload", Json { { "directory", payload.directory }, { "clangd", payload.clangd }, { "clangdVersion", payload.clangdVersion },
                            { "kit", payload.kit }, { "kitNotice", payload.kitNotice }, { "platform", payload.platform }, { "corrupt", payloadCorrupt } } },
        { "roots", std::move(roots) },
        { "settings", std::move(settings) },
        { "logTail", log::recent(300) },
    };
}

std::string open_log_file(std::string_view kind, std::size_t keep) {
    std::error_code ignored;
    const std::string directory { base::join_path(base::cache_directory(), "logs") };
    (void)std::filesystem::create_directories(directory, ignored);
    // The newest first: names carry their start time, so they sort by it.
    std::map<std::string, std::vector<std::string>, std::greater<>> logs;
    const std::string prefix { std::string { kind } + "-" };
    for (auto& path : base::children(directory)) {
        std::string name { base::file_name(path) };
        if (!name.starts_with(prefix)) continue;
        const std::size_t log { name.find(".log") };
        if (log == std::string::npos) continue;
        logs[name.substr(0, log)].push_back(std::move(path));
    }
    std::size_t kept { 0 };
    for (const auto& [base, files] : logs) {
        if (++kept < keep) continue;
        for (const auto& file : files) std::filesystem::remove_all(file, ignored);
    }
    const std::string path { base::join_path(directory, std::format("{}-{}-{}.log", kind, utc_now("{:%Y%m%d-%H%M%S}"), os::random::hex(2).value_or(std::string {}))) };
    return log::add_file(path) ? path : std::string {};
}

} // namespace mcppls::orchestrator
