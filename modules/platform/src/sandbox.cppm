// Whether this process runs under a system-call translator, which changes what a program may do
// (termux's PRoot answers ENOSYS to calls a kernel would accept), and so belongs in a report of why
// something did not work. The classification is mcpplibs.os.linux's `translator`.
export module mcppls.platform.sandbox;

import std;

export namespace mcppls::platform {

// "proot" when the process is traced by PRoot, else empty; computed once. Empty on a system with
// no such notion (macOS, Windows) and in an ok image, which asks no system's own tables.
const std::string& sandbox();

} // namespace mcppls::platform
