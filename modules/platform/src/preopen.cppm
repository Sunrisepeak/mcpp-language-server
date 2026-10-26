// The directories openkal supplied this program, which every absolute name lies beneath
// (mcpplibs.os.fs `locate`): named for a report and for the startup check.
export module mcppls.platform.preopen;

import std;

export namespace mcppls::platform {

// Whether some supplied directory is the file system's root, by name. On a POSIX system openkal
// supplies "/" and every program path resolves beneath it; when none is named "/" every start fails
// as "beneath no directory this program was given", which is worth saying at startup rather than at
// the first spawn. A system with no root directory (Windows) is never asked: true.
bool has_root_preopen(std::span<const std::string> names);
// The supplied directories' names, as openkal reports them.
std::vector<std::string> preopen_names();

} // namespace mcppls::platform
