// Where mcppls finds the user's home and keeps its own state, over mcpplibs.os `dirs`.
export module mcppls.base.home;

import std;

export namespace mcppls::base {

// The user's home, in this spelling; on a system that names none, the root of the system drive
// ("C:/" on Windows, "/" elsewhere), so a path built on it is absolute.
std::string home_directory();

// Where mcppls keeps its state: MCPPLS_CACHE_DIR when it is set, else `mcppls` in the user's cache
// directory (mcpplibs.os `dirs::cache()`), else in the system's convention under the home.
std::string cache_directory();

} // namespace mcppls::base
