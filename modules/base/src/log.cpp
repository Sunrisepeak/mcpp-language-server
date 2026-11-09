module mcppls.base.log;

import std;
import mcpplibs.os;

namespace mcppls::base::log {

namespace {

std::atomic<Level> gLevel { Level::info };

// The file every line also goes to, and the lines kept for a report.
struct LogFile {
    std::string path;
    std::ofstream stream;
    std::uintmax_t written { 0 };
    std::uintmax_t maxBytes { 0 };
    int keep { 0 };
};
constexpr std::size_t RECENT_LINES { 500 };

// The log's state is never destroyed: a detached thread (a daemon's connection, a reader of a
// child's output) may write a line while the process ends, after static objects are destroyed,
// and it then finds the state whole rather than freed.
struct State {
    std::mutex mutex;
    std::function<void(std::string_view)> sink;
    std::optional<LogFile> file;
    std::deque<std::string> recent;
};
State& state() {
    static State* const instance { new State };
    return *instance;
}

void rotate(LogFile& file) {
    file.stream.close();
    std::error_code ignored;
    for (int index { file.keep }; index >= 1; --index) {
        const std::filesystem::path from { index == 1 ? file.path : std::format("{}.{}", file.path, index - 1) };
        std::filesystem::rename(from, std::format("{}.{}", file.path, index), ignored);
    }
    if (file.keep <= 0) std::filesystem::remove(file.path, ignored);
    file.stream.open(std::filesystem::path { file.path }, std::ios::out | std::ios::trunc);
    file.written = 0;
}

std::string_view name_of(Level level) {
    switch (level) {
    case Level::debug: return "debug";
    case Level::info: return "info";
    case Level::warning: return "warning";
    case Level::error: return "error";
    case Level::off: return "off";
    }
    return "?";
}

} // namespace

void set_level(Level level) { gLevel.store(level); }

void set_sink(std::function<void(std::string_view line)> sink) {
    auto& s { state() };
    std::lock_guard lock { s.mutex };
    s.sink = std::move(sink);
}

Level level() { return gLevel.load(); }

bool enabled(Level level) {
    return level != Level::off && static_cast<int>(level) >= static_cast<int>(gLevel.load());
}

void write(Level level, std::string_view message) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const std::string line { std::format("mcppls {:%FT%TZ} [{}] {}\n", now, name_of(level), message) };
    auto& s { state() };
    std::lock_guard lock { s.mutex };
    s.recent.emplace_back(line.substr(0, line.size() - 1));
    if (s.recent.size() > RECENT_LINES) s.recent.pop_front();
    if (s.file && s.file->stream) {
        s.file->stream << line;
        s.file->stream.flush();
        s.file->written += line.size();
        if (s.file->maxBytes > 0 && s.file->written >= s.file->maxBytes) rotate(*s.file);
    }
    if (s.sink) {
        s.sink(line);
        return;
    }
    (void)mcpplibs::os::stdio::write_error(line);
}

bool add_file(std::string_view path, std::uintmax_t maxBytes, int keep) {
    auto& s { state() };
    std::lock_guard lock { s.mutex };
    LogFile file;
    file.path = std::string { path };
    file.maxBytes = maxBytes;
    file.keep = keep;
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path { file.path }.parent_path(), ignored);
    file.stream.open(std::filesystem::path { file.path }, std::ios::out | std::ios::app);
    if (!file.stream) return false;
    file.written = std::filesystem::file_size(file.path, ignored);
    if (file.written == static_cast<std::uintmax_t>(-1)) file.written = 0;
    s.file = std::move(file);
    return true;
}

std::string file_path() {
    auto& s { state() };
    std::lock_guard lock { s.mutex };
    return s.file ? s.file->path : std::string {};
}

std::vector<std::string> recent(std::size_t limit) {
    auto& s { state() };
    std::lock_guard lock { s.mutex };
    const std::size_t count { std::min(limit, s.recent.size()) };
    return { s.recent.end() - static_cast<std::ptrdiff_t>(count), s.recent.end() };
}

std::optional<Level> parse_level(std::string_view name) {
    if (name == "debug" || name == "verbose") return Level::debug;
    if (name == "info") return Level::info;
    if (name == "warning" || name == "warn") return Level::warning;
    if (name == "error") return Level::error;
    if (name == "off") return Level::off;
    return std::nullopt;
}

} // namespace mcppls::base::log
