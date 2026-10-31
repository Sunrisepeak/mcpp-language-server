// Packaging: the payload (the server, the engine of every platform, the openkal kit), the editor
// extensions built around it, and removing what was installed.
//
//   mcppls-devtools payload [--platforms P,...] [--server-image OKB --server-tree TREE | --dev]
//                            [--clangd P=DIR ...] [--kit DIR] [--cache DIR] [--out DIR]
//                            [--verify DIR] [--from DIR] [--only clangd --platform P]
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
