// Repository invariants, checked by `mcppls-devtools check <verb>`, run as CI's first step
// (tooling architecture §7 and §5.8). Every verb here answers a question about the SOURCE tree
// except `binary`, which answers one about a built artifact.
//
//   mcppls-devtools check os-surface            the platform surface is six constants, ported
//                                                from tools/check_os_surface.py
//   mcppls-devtools check layers                every member depends only on lower layers (§3.1)
//   mcppls-devtools check versions              version --check, plus every third-party dep uses
//                                                `.workspace = true`
//   mcppls-devtools check scripts                every tracked *.py/*.sh is in scripts.allow
//   mcppls-devtools check binary --server PATH  the server's symbol table carries no archive or
//                                                TLS symbol (F1's final backstop)
//   mcppls-devtools check all                   every source-only check above (not `binary`,
//                                                which needs a built server)
export module mcppls.devtools.check;

import std;
import mcpplibs.cmdline;
import mcppls.base.error;

export namespace mcppls::devtools::check {

// One check's outcome: `problems` is what a failing exit code is about; `notes` is informational
// (an "ok" line, an absent-but-optional site) and never affects `ok`.
struct Report {
    bool ok { true };
    std::vector<std::string> problems;
    std::vector<std::string> notes;
};

base::Result<Report> os_surface(const std::string& root);
base::Result<Report> layers(const std::string& root);
base::Result<Report> versions(const std::string& root);
base::Result<Report> scripts(const std::string& root);
base::Result<Report> binary(const std::string& serverPath);
// docs/93-devtools.md names every command `commands` holds, and names no command that is not one.
base::Result<Report> docs(const std::string& root, const std::vector<std::string>& commands);
// The commands a `--help` text lists under SUBCOMMANDS.
std::vector<std::string> commands_in_help(std::string_view help);

} // namespace mcppls::devtools::check

export namespace mcppls::devtools {

mcpplibs::cmdline::App check_command(bool& handled, int& status);

} // namespace mcppls::devtools
