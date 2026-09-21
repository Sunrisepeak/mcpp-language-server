// Well-known directories: where this program keeps state, where temporary
// files go, and the user's home.
export module mcppls.platform.dirs;

import std;

export namespace mcppls::platform::dirs {

std::string home_directory();
// <user cache>/mcppls. MCPPLS_CACHE_DIR overrides the whole path.
std::string cache_directory();
std::string temp_directory();

} // namespace mcppls::platform::dirs
