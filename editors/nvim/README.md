# mcppls for Neovim

C++20/23 named modules in Neovim: go to definition, hover, references and completion across
modules, on any compiler, through Neovim's own LSP client. The plugin finds `mcppls`, starts it for
C and C++ buffers, and keeps what the server reports about the project for your statusline.

Neovim 0.10 or later. CI runs it on 0.10.4, 0.11.5 and 0.12.5.

## The server

The plugin starts `mcppls serve`. It looks for the server in this order:

1. `server` given to `setup()`
2. `mcppls` on `PATH`
3. `<user data>/mcppls/payload/bin/mcppls`, where `mcpp run -p devtools -- extension --install`
   puts it (`~/.local/share` on Linux, `~/Library/Application Support` on macOS, `%LOCALAPPDATA%`
   on Windows)

The simplest way to get it: download `payload-<platform>.tar.gz` (`linux-x64`, `darwin-arm64`,
`win32-x64`) from the [release page](https://github.com/Sunrisepeak/mcpp-language-server/releases),
unpack it, and put its `payload/bin/` on `PATH`. The payload carries a pinned clangd and the
semantic kit, so nothing else is needed.

## Install the plugin

The plugin is the `editors/nvim` directory of this repository.

**lazy.nvim**

```lua
{
  'Sunrisepeak/mcpp-language-server',
  config = function(plugin)
    vim.opt.rtp:append(plugin.dir .. '/editors/nvim')
    require('mcppls').setup()
  end,
}
```

**Without a plugin manager**

```lua
-- after: git clone https://github.com/Sunrisepeak/mcpp-language-server ~/.local/share/mcppls-src
vim.opt.rtp:append(vim.fn.expand('~/.local/share/mcppls-src/editors/nvim'))
require('mcppls').setup()
```

**Neovim 0.11 and later** can use the built-in configuration mechanism instead of `setup()`:
with `editors/nvim` on the runtimepath, `vim.lsp.enable('mcppls')` reads `lsp/mcppls.lua`, which is
the same configuration.

## One C++ server, not two

mcppls runs clangd itself, with the module database it built. Do not also start clangd (or ccls)
for C and C++: two servers over one file means two engines answering. If one is attached to a
buffer mcppls serves, the plugin says so once, naming it. With nvim-lspconfig that means not
calling `lspconfig.clangd.setup()`; with 0.11's mechanism, `vim.lsp.enable('clangd', false)`.

## Options

```lua
require('mcppls').setup({
  server = nil,              -- path to mcppls; default: PATH, then the installed payload
  filetypes = { 'c', 'cpp' },
  root_markers = { 'mcpp.toml', 'CMakeLists.txt', 'compile_commands.json', '.git' },
  init_options = {           -- passed to the server; see docs/30-settings.md
    -- compiler = 'clang++',
    -- semanticKit = 'auto',  -- or 'off'
  },
  detect_conflicts = true,   -- say so when clangd or ccls is attached beside mcppls
})
```

The root is the nearest directory with a root marker, else the current directory. `.cppm`, `.ixx`,
`.mpp`, `.ccm` and `.cxxm` files are C++.

## Commands and statusline

| Command | |
|---|---|
| `:McpplsStatus` | The project's build description, engine and any issues the server reports |
| `:McpplsRestart` | Restart the server |
| `:McpplsReload` | Read the build description again, e.g. after running the build tool by hand |

`require('mcppls').status()` is a statusline component: `mcppls ready · mcpp L1`,
`mcppls preparing 12/40`, `mcppls degraded · inferred L4 · 1 issue`. The raw notification is
`require('mcppls').status_data()`, and a `User McpplsStatus` autocmd fires on each one.

```lua
vim.o.statusline = '%f %= %{v:lua.require("mcppls").status()} '
```

## Logs

The server's own log, with each line's level, is in `~/.cache/mcppls/logs` (the first line of a
session names the file). Neovim also writes the server's stderr to its own LSP log
(`:lua vim.cmd.edit(vim.lsp.get_log_path())`), where, by Neovim's convention for every server, each
of those lines is tagged ERROR: "ERROR messages containing stderr only indicate that the log was
sent to stderr" (`:help lsp-log`). They are not errors; the server's own log has their real level.

## Tests

```bash
MCPPLS_SERVER=<payload>/bin/mcppls nvim --headless --clean -l editors/nvim/tests/smoke.lua setup
MCPPLS_SERVER=<payload>/bin/mcppls nvim --headless --clean -l editors/nvim/tests/smoke.lua enable
```

On a copy of the `inferred` conformance fixture: the server attaches and reports ready, definition
(of a symbol and of a module name), hover, references across modules and completion answer, nothing
is shown to the user, and a second C++ server is named once. xlings installs and switches the
Neovim versions: `xlings install nvim@0.10.4`, `xlings use nvim 0.10.4`.
