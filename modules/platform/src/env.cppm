// The process environment over openkal.env.
export module mcppls.platform.env;

import std;

export namespace mcppls::platform::env {

// Value of a variable; names compare case-insensitively on Windows.
std::optional<std::string> get(std::string_view name);
// Every variable as "NAME=value", in the order the environment reports them.
std::vector<std::string> variables();
// PATH split into normalized directory paths.
std::vector<std::string> search_path();
// An executable found by name on PATH, or checked directly when `name` is a path.
// The platform executable suffix is added when missing.
std::optional<std::string> find_executable(std::string_view name);
// The same, on the directories of `pathList` (a PATH value), for a program started in another environment.
std::optional<std::string> find_executable(std::string_view name, std::string_view pathList);
// This program's own command-line arguments, argv[0] included.
std::vector<std::string> arguments();

} // namespace mcppls::platform::env
