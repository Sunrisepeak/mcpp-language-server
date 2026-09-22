-- Neovim 0.11 and later: `vim.lsp.enable('mcppls')` reads this file from the runtimepath. The
-- configuration is the one require('mcppls').setup() starts, so the two ways in cannot drift.
local mcppls = require('mcppls')
mcppls._define_commands()
return mcppls.config()
