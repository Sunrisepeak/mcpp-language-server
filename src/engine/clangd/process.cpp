module mcppls.engine.clangd.process;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.lsp.connection;

namespace mcppls::engine::clangd {

std::vector<std::string> clangd_arguments(const ProcessConfig& config) {
    std::vector<std::string> arguments {
        "--experimental-modules-support",
        "--use-dirty-headers",
        "--compile-commands-dir=" + config.databaseDirectory,
        "--background-index",
        "--header-insertion=never",
        "--pretty=false",
        config.verboseLog ? "--log=verbose" : "--log=error",
    };
    const bool workersGiven { std::ranges::any_of(config.extraArguments, [](const std::string& argument) { return argument.starts_with("-j"); }) };
    if (config.workers > 0 && !workersGiven) arguments.push_back(std::format("-j={}", config.workers));
    arguments.insert(arguments.end(), config.extraArguments.begin(), config.extraArguments.end());
    return arguments;
}

std::optional<ModuleFailure> parse_module_failure(std::string_view line) {
    static constexpr std::string_view MARKER { "Failed to build module " };
    const std::size_t marker { line.find(MARKER) };
    if (marker == std::string_view::npos) return std::nullopt;
    std::string_view rest { line.substr(marker + MARKER.size()) };
    const std::size_t semicolon { rest.find(';') };
    if (semicolon == std::string_view::npos || semicolon == 0) return std::nullopt;
    ModuleFailure failure;
    failure.module = std::string { base::trim(rest.substr(0, semicolon)) };
    std::string_view reason { rest.substr(semicolon + 1) };
    if (const std::size_t due { reason.find("due to ") }; due != std::string_view::npos) reason = reason.substr(due + 7);
    if (const std::size_t hint { reason.find(" Use '--log=verbose'") }; hint != std::string_view::npos) reason = reason.substr(0, hint);
    reason = base::trim(reason);
    if (reason.ends_with('.')) reason.remove_suffix(1);
    failure.reason = std::string { reason };
    static constexpr std::string_view COMPILE { "Failed to compile " };
    if (reason.starts_with(COMPILE)) failure.failedSource = std::string { base::trim(reason.substr(COMPILE.size())) };
    return failure;
}

base::log::Level clangd_log_level(std::string_view line) {
    if (line.size() >= 2 && line[1] == '[') {
        switch (line[0]) {
        case 'E': return base::log::Level::warning;
        case 'I':
        case 'V':
        case 'D': return base::log::Level::debug;
        default: break;
        }
    }
    return base::log::Level::info;
}

bool loader_failure(std::string_view line) {
    // clangd's own log lines start with a severity letter and a timestamp ("E[10:31:02.1] ..."); a
    // loader's never do, and one of them quoting these words is a message about a file, not this.
    if (line.size() >= 2 && line[1] == '[') return false;
    static constexpr std::array<std::string_view, 6> MARKERS {
        "error while loading shared libraries",   // glibc: a library missing
        "not found (required by",                 // glibc: a symbol version missing ("version `GLIBCXX_3.4.30' not found")
        "Error loading shared library",           // musl
        "Error relocating",                       // musl: a symbol missing
        "Library not loaded",                     // dyld
        "Symbol not found",                       // dyld
    };
    return std::ranges::any_of(MARKERS, [&](std::string_view marker) { return line.find(marker) != std::string_view::npos; });
}

FailureKind failure_kind(const ModuleFailure& failure) {
    if (failure.reason.find("Don't get the module unit") != std::string::npos) return FailureKind::unresolved;
    if (failure.reason.starts_with("Failed to compile")) return FailureKind::compile;
    return FailureKind::other;
}

std::string parse_clangd_version(std::string_view output) {
    for (auto line : base::split_lines(output)) {
        const std::size_t marker { line.find("clangd version ") };
        if (marker == std::string_view::npos) continue;
        std::string_view rest { line.substr(marker + 15) };
        return std::string { rest.substr(0, rest.find_first_of(" \t\r")) };
    }
    return {};
}

base::Result<void> ClangdProcess::start(const ProcessConfig& config, MessageHandler onMessage, ClosedHandler onClosed, LogHandler onLog) {
    stop(std::chrono::milliseconds { 200 });
    config_ = config;
    platform::SpawnOptions options;
    options.program = config.executable;
    options.arguments = clangd_arguments(config);
    options.workDirectory = config.workDirectory;
    options.ownUnit = true;
    auto connection = lsp::Connection::start(std::move(options), std::move(onMessage), std::move(onClosed), std::move(onLog));
    if (!connection) return std::unexpected { connection.error() };
    connection_ = std::move(*connection);
    return {};
}

base::Result<void> ClangdProcess::send(const nlohmann::json& message) {
    if (!connection_) return base::fail("engine-stopped", "clangd is not running");
    return connection_->send(message);
}

void ClangdProcess::stop(std::chrono::milliseconds grace) {
    if (!connection_) return;
    connection_->stop(grace);
    connection_.reset();
}

bool ClangdProcess::running() const { return connection_ && !connection_->closed(); }

std::function<std::optional<double>()> ClangdProcess::cpu_reader() const {
    if (!running()) return {};
    const auto pid = connection_->native_pid();
    if (!pid) return {};
    return [pid = *pid] { return platform::cpu_seconds(pid); };
}

} // namespace mcppls::engine::clangd
