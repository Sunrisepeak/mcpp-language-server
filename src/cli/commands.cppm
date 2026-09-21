// The mcppls command line (overall design 8.1), and the composition root that gives every workspace
// root its engines:
//   mcppls [serve] [--payload DIR] [--clangd PATH] [--kit DIR] [--engine clangd|none] [--untrusted] [--log-level L]
//   mcppls check <file> [--payload DIR] [--clangd PATH] [--kit DIR]
//   mcppls model [--root DIR] [--export s1|compile-commands|engine] [--payload DIR] [--kit DIR]
//   mcppls version
export module mcppls.cli.commands;

import std;

export namespace mcppls::cli {

int run(int argc, char* argv[]);

} // namespace mcppls::cli
