module mcppls.base.log;

import std;
import openkal.stream;

namespace mcppls::base::log {

namespace {

std::atomic<Level> gLevel { Level::info };
std::mutex gWriteMutex;
std::function<void(std::string_view)> gSink;

// The file every line also goes to, and the lines kept for a report.
struct LogFile {
    std::string path;
    std::ofstream stream;
    std::uintmax_t written { 0 };
    std::uintmax_t maxBytes { 0 };
    int keep { 0 };
};
std::optional<LogFile> gFile;
constexpr std::size_t RECENT_LINES { 500 };
std::deque<std::string> gRecent;

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
    std::lock_guard lock { gWriteMutex };
    gSink = std::move(sink);
}

Level level() { return gLevel.load(); }

bool enabled(Level level) {
    return level != Level::off && static_cast<int>(level) >= static_cast<int>(gLevel.load());
}

void write(Level level, std::string_view message) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const std::string line { std::format("mcppls {:%FT%TZ} [{}] {}\n", now, name_of(level), message) };
    std::lock_guard lock { gWriteMutex };
    gRecent.emplace_back(line.substr(0, line.size() - 1));
    if (gRecent.size() > RECENT_LINES) gRecent.pop_front();
    if (gFile && gFile->stream) {
        gFile->stream << line;
        gFile->stream.flush();
        gFile->written += line.size();
        if (gFile->maxBytes > 0 && gFile->written >= gFile->maxBytes) rotate(*gFile);
    }
    if (gSink) {
        gSink(line);
        return;
    }
    std::size_t done { 0 };
    while (done < line.size()) {
        const auto written = kal_stream_write(kal_stderr(), line.data() + done, line.size() - done);
        if (written <= 0) break;
        done += static_cast<std::size_t>(written);
    }
    kal_stream_flush(kal_stderr());
}

bool add_file(std::string_view path, std::uintmax_t maxBytes, int keep) {
    std::lock_guard lock { gWriteMutex };
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
    gFile = std::move(file);
    return true;
}

std::string file_path() {
    std::lock_guard lock { gWriteMutex };
    return gFile ? gFile->path : std::string {};
}

std::vector<std::string> recent(std::size_t limit) {
    std::lock_guard lock { gWriteMutex };
    const std::size_t count { std::min(limit, gRecent.size()) };
    return { gRecent.end() - static_cast<std::ptrdiff_t>(count), gRecent.end() };
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
