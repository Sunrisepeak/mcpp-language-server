# Changelog

## 0.0.8

- The completion list opens while you type in C and C++ files, alongside Copilot's or another inline
  completion's grey text. VS Code 1.125 and later otherwise keep it closed while grey text shows and
  open it only after you stop typing (`editor.quickSuggestions` defaults to
  `offWhenInlineCompletions`); the extension sets `{"other": "on"}` for `[cpp]` and `[c]`. A value you
  set under `"[cpp]"` / `"[c]"` wins; one set for every language is overridden, and the log says so once.
- **Export Diagnostic Bundle** records the editor settings that decide whether completion shows.
- Writing a module with autosave keeps its completion and real diagnostics (the server no longer takes
  a module that does not compile from clangd), and completion is never empty.

## 0.0.7

- Faster answers while you type: completion waits for clangd at most a second before mcppls answers
  with the file's words (an incomplete list, asked again as you type), background indexing waits for
  a pause, and restarts wait until you stop typing.
- **Reset This Workspace's Cache** in the palette: the server stops, removes the workspace's cache and
  starts again.
- When mcppls cannot recover by itself (clangd crashing again and again, a corrupt payload, an
  incompatible clangd, preparation that never finishes) the server saves a diagnostic bundle and the
  editor shows one non-modal notification: **Report Issue…** (reveals the bundle and opens the bug
  report form prefilled with versions, editor and system), **Restart Server**, **Reset This
  Workspace's Cache**, **Turn Off in This Workspace**, **Show Logs**.
- A server process that keeps dying (three crashes in three minutes) is no longer restarted
  forever: the extension writes a small crash report under its storage and shows the same
  notification. Restarting by hand or turning the server off is never counted as a crash.
- `mcppls.enable`: `false` in a workspace's settings keeps the server from starting there. The status
  bar item says **C++ Modules: off in this workspace** and turns it back on; **Turn Off in This
  Workspace** and **Turn On in This Workspace** are in the palette.

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
