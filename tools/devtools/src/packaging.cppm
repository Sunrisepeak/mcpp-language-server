// Packaging: a payload (the server, a trimmed clangd, the semantic kit), the editor extensions
// built around it, and removing what was installed.
//
//   mcppls-devtools payload [--platform P] [--server PATH | --dev] [--clangd DIR] [--kit DIR]
//                            [--cache DIR] [--out DIR] [--verify DIR] [--from DIR] [--only clangd]
//   mcppls-devtools extension [--editor vscode|zed|clion|all] [--install] [--payload DIR] [--out FILE]
//   mcppls-devtools uninstall [--editor ...]
export module mcppls.devtools.packaging;

import std;
import mcpplibs.cmdline;

export namespace mcppls::devtools {

mcpplibs::cmdline::App payload_command(bool& handled, int& status);
mcpplibs::cmdline::App extension_command(bool& handled, int& status);
mcpplibs::cmdline::App uninstall_command(bool& handled, int& status);

} // namespace mcppls::devtools
