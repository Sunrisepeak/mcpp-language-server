// Who this process runs as, for what a report or a bundle must not name (issue #23 fix plan F18):
// read from the environment and a few system files, never by starting a program, so the event loop
// may ask for it.
export module mcppls.bundle.identity;

import std;
import mcppls.bundle.redact;

export namespace mcppls::bundle {

// The home directory as set and as the file system names it (a symbolic link, an 8.3 alias
// resolved), the login name and the home directory's last component, and the machine's names.
Identity current_identity();

// A copy of `identity` that also hides `roots` as <workspace>, <workspace-2>, ...
Identity hiding_workspaces(Identity identity, std::vector<std::string> roots);

} // namespace mcppls::bundle
