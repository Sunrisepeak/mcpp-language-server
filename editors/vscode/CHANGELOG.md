# Changelog

## 0.0.1

- First public release: the mcppls language server with a pinned clangd 23.1 and the semantic kit
  built in, so C++20 modules and `import std` work on any compiler, and with none.
- The server's log reaches the C++ Modules output at the level the server gave each line: a
  healthy start reads as information, not as a column of errors.
- A status item that names the build description, the engine and anything degraded; **Collect
  Diagnostic Report** for bug reports.
- Module-name navigation, `import` completion and module diagnostics alongside clangd's features.
- A one-time offer to turn off other C++ extensions' language features in the workspace, and on
  macOS to install the Command Line Tools when the SDK is missing.

The full list is in the repository's [CHANGELOG](https://github.com/Sunrisepeak/mcpp-language-server/blob/main/CHANGELOG.md).
