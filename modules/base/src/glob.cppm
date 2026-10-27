// Glob patterns in the syntax of LSP 3.17/3.18 (`*`, `**`, `?`, `{a,b}`, `[...]`, `[!...]`), which S2
// uses for the inputs a producer asks a consumer to watch. Paths use '/' separators.
export module mcppls.base.glob;

import std;
import mcpplibs.os;

export namespace mcppls::base {

// Whether `path` matches `pattern`; both are '/'-separated and compared segment by segment.
bool glob_match(std::string_view pattern, std::string_view path, bool caseInsensitive = mcpplibs::os::conventions().case_insensitive_names);

} // namespace mcppls::base
