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
        // Fix plan F17.1 (D3): info, not error, so an incident carries what clangd was doing. Its info
        // lines go to the ring buffer and the debug log only, never to the default log.
        config.verboseLog ? "--log=verbose" : "--log=info",
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

namespace {

// clangd's own log lines start with a severity letter and a timestamp ("E[10:31:02.1] ...").
bool has_severity(std::string_view line) { return line.size() >= 2 && line[1] == '[' && std::string_view { "EIVD" }.contains(line[0]); }

// What follows the timestamp of a line with a severity.
std::string_view message_of(std::string_view line) {
    const std::size_t close { line.find("] ") };
    return close == std::string_view::npos ? line : line.substr(close + 2);
}

// "D:\p\a.cpp:1:10: fatal error: 'x.h' file not found" -> "fatal error: 'x.h' file not found", not the driver's;
// "clang++: error: LTO requires -fuse-ld=lld" -> "error: LTO requires -fuse-ld=lld", the driver's: what comes before
// the error names a program, not a place in a file.
struct ErrorPart {
    std::string_view text;
    bool driver { false };
};
std::optional<ErrorPart> error_part(std::string_view line) {
    for (const std::string_view marker : { std::string_view { "fatal error: " }, std::string_view { "error: " } }) {
        const std::size_t at { line.find(marker) };
        if (at == std::string_view::npos) continue;
        std::string_view before { base::trim(line.substr(0, at)) };
        if (before.ends_with(':')) before.remove_suffix(1);
        const bool driver { before.empty() || (before.find(':') == std::string_view::npos && before.find(' ') == std::string_view::npos) };
        return ErrorPart { base::trim(line.substr(at)), driver };
    }
    return std::nullopt;
}

} // namespace

LogReader::Read LogReader::read(std::string_view line) {
    Read read;
    const std::string_view trimmed { base::trim(line) };
    // A crash context: its lines come one after another, whatever else clangd's threads print.
    static constexpr std::string_view SIGNALLED_ACTION { "Signalled during AST worker action: " };
    static constexpr std::string_view SIGNALLED_PREAMBLE { "Signalled while building preamble" };
    if (trimmed.starts_with(SIGNALLED_ACTION) || trimmed.starts_with(SIGNALLED_PREAMBLE)) {
        crash_ = CrashContext { trimmed.starts_with(SIGNALLED_ACTION) ? std::string { trimmed.substr(SIGNALLED_ACTION.size()) } : std::string { "building preamble" },
                                {}, {} };
        inCrash_ = true;
        read.important = true;
        return read;
    }
    if (inCrash_ && line.starts_with("  ")) {
        read.important = true;
        if (trimmed.starts_with("Filename: ") && crash_ && crash_->file.empty()) {
            crash_->file = std::string { base::trim(trimmed.substr(10)) };
            read.crash = crash_;
        }
        return read;
    }
    inCrash_ = false;
    if (trimmed.starts_with("Exception Code: ")) {
        read.important = true;
        if (crash_) {
            crash_->exception = std::string { base::trim(trimmed.substr(16)) };
            read.crash = crash_;
        }
        return read;
    }
    if (trimmed.starts_with("PLEASE submit a bug report") || trimmed.starts_with("Stack dump:")) {
        read.important = true;
        return read;
    }
    // A scan failure: its header, the lines without a severity that continue it, and its closing line.
    static constexpr std::string_view SCANNING { "Scanning modules dependencies for " };
    static constexpr std::string_view FAILED { " failed: " };
    if (has_severity(line)) {
        const std::string_view message { message_of(line) };
        if (message.starts_with("The command line the scanning tool use is:")) {
            read.important = scan_.has_value();
            read.scanFailure = std::move(scan_);
            scan_.reset();
            return read;
        }
        read.scanFailure = std::move(scan_);   // one that never got its closing line
        scan_.reset();
        if (message.starts_with(SCANNING)) {
            const std::string_view rest { message.substr(SCANNING.size()) };
            const std::size_t failed { rest.find(FAILED) };
            if (failed != std::string_view::npos) {
                const std::string_view first { rest.substr(failed + FAILED.size()) };
                const auto error = error_part(first);
                scan_ = ScanFailure { std::string { rest.substr(0, failed) }, std::string { error ? error->text : base::trim(first) }, error && error->driver };
                scanHasError_ = error.has_value();
                read.important = true;
            }
        }
        return read;
    }
    if (scan_) {
        read.important = true;
        if (!scanHasError_) {
            if (const auto error = error_part(line)) {
                scan_->reason = std::string { error->text };
                scan_->driver = error->driver;
                scanHasError_ = true;
            }
        }
    }
    return read;
}

std::optional<ScanFailure> LogReader::finish() {
    auto scan = std::move(scan_);
    scan_.reset();
    return scan;
}

void LogRing::add(std::string_view line) {
    const std::lock_guard lock { mutex_ };
    lines_.emplace_back(line);
    bytes_ += line.size() + 1;
    while (!lines_.empty() && (lines_.size() > maxLines_ || bytes_ > maxBytes_)) {
        bytes_ -= lines_.front().size() + 1;
        lines_.pop_front();
        ++dropped_;
    }
}

std::string LogRing::text() const {
    const std::lock_guard lock { mutex_ };
    std::string text;
    text.reserve(bytes_ + 64);
    if (dropped_ > 0) text += std::format("[{} earlier lines are not kept]\n", dropped_);
    for (const auto& line : lines_) {
        text += line;
        text += '\n';
    }
    return text;
}

std::size_t LogRing::size() const {
    const std::lock_guard lock { mutex_ };
    return lines_.size();
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

std::optional<std::int64_t> ClangdProcess::pid() const {
    if (!connection_) return std::nullopt;
    return connection_->native_pid();
}

std::optional<int> ClangdProcess::exit_code() {
    if (!connection_) return std::nullopt;
    return connection_->exit_code();
}

} // namespace mcppls::engine::clangd
