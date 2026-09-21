// Building an mcppls-kit semantic kit by itself, outside a payload.
//
//   mcppls-devtools kit --platform linux-x64|win32-x64|darwin-arm64 --out DIR [--cache DIR] [--work DIR] [--jobs N]
//
// This is the CLI shell around mcppls.pack.kit::build(): argument parsing, finding the repository
// and its lock file, and reporting the result. Everything that runs cmake, ninja or a compiler
// lives in the library (tooling architecture §4), so `devtools payload` calls the same function
// rather than shelling out to this command.
export module mcppls.devtools.kit;

import std;
import mcpplibs.cmdline;

export namespace mcppls::devtools {

mcpplibs::cmdline::App kit_command(bool& handled, int& status);

} // namespace mcppls::devtools
