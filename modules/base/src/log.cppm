// Diagnostic logging to standard error, each line with its time, and optionally to a rotated file. The
// protocol owns standard output, so nothing here may ever write there.
export module mcppls.base.log;

import std;

export namespace mcppls::base::log {

enum class Level { debug, info, warning, error, off };

void set_level(Level level);
Level level();
bool enabled(Level level);
void write(Level level, std::string_view message);
// Where lines go instead of standard error, for a process nobody reads the standard error of (the daemon).
void set_sink(std::function<void(std::string_view line)> sink);
// Every line also goes to this file, which outlives the editor's output (robustness design O2). It is rotated
// when it passes `maxBytes`: the previous `keep` files stay beside it as <path>.1, <path>.2, ... False when the
// file cannot be opened.
bool add_file(std::string_view path, std::uintmax_t maxBytes = 5 * 1024 * 1024, int keep = 2);
std::string file_path();
// The most recent lines written, oldest first, at most `limit` of them (robustness design O3).
std::vector<std::string> recent(std::size_t limit);
std::optional<Level> parse_level(std::string_view name);

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::debug)) write(Level::debug, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::info)) write(Level::info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warning(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::warning)) write(Level::warning, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::error)) write(Level::error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace mcppls::base::log
