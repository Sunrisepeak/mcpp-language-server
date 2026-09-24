-- End to end in a headless Neovim: the plugin starts mcppls on a copy of the `inferred` conformance
-- fixture (C++ modules, no build system) and the answers that matter come back through Neovim's
-- own LSP client.
--
--   MCPPLS_SERVER=<payload>/bin/mcppls nvim --headless --clean -l editors/nvim/tests/smoke.lua [setup|enable]
--
-- `setup` (the default) goes through require('mcppls').setup(); `enable` through
-- vim.lsp.enable('mcppls') and lsp/mcppls.lua, which needs Neovim 0.11. Exits 0 when every check
-- passes; each check prints PASS or FAIL with what it saw.

local uv = vim.uv or vim.loop
local sep = package.config:sub(1, 1)
local here = vim.fn.fnamemodify(debug.getinfo(1, 'S').source:sub(2), ':p:h')
local plugin = vim.fn.fnamemodify(here, ':h')
local repo = vim.fn.fnamemodify(plugin, ':h:h')
local mode = arg[1] or 'setup'

local server = os.getenv('MCPPLS_SERVER')
if not server or server == '' then
  io.stderr:write('MCPPLS_SERVER is not set\n')
  os.exit(2)
end

local failures = 0
local function check(name, ok, detail)
  print(string.format('%s %s%s', ok and 'PASS' or 'FAIL', name, detail and (': ' .. tostring(detail)) or ''))
  if not ok then
    failures = failures + 1
  end
end

-- A private copy, so nothing the server writes lands in the repository.
local function copy(from, to)
  vim.fn.mkdir(to, 'p')
  for name, kind in vim.fs.dir(from) do
    local a, b = from .. sep .. name, to .. sep .. name
    if kind == 'directory' then
      copy(a, b)
    elseif name ~= 'scenario.json' then
      uv.fs_copyfile(a, b)
    end
  end
end
local workspace = vim.fn.tempname() .. '-mcppls-nvim'
copy(table.concat({ repo, 'conformance', 'fixtures', 'inferred' }, sep), workspace)
vim.fn.chdir(workspace)

vim.opt.runtimepath:prepend(plugin)
local mcppls = require('mcppls')
if mode == 'enable' then
  if vim.fn.has('nvim-0.11') == 0 then
    print('SKIP vim.lsp.enable needs Neovim 0.11')
    os.exit(0)
  end
  -- lsp/mcppls.lua finds the server on PATH, as a user's would.
  vim.env.PATH = vim.fs.dirname(server) .. (sep == '\\' and ';' or ':') .. vim.env.PATH
  vim.lsp.enable('mcppls')
else
  mcppls.setup({ server = server })
end

-- Every message shown, so the test can say none was shown that should not be.
local shown = {}
local notify = vim.notify
vim.notify = function(message, level, ...)
  shown[#shown + 1] = message
  return notify(message, level, ...)
end
vim.lsp.handlers['window/showMessage'] = function(_, params)
  shown[#shown + 1] = params.message
end

vim.cmd.edit('src' .. sep .. 'main.cpp')
check('main.cpp is C++', vim.bo.filetype == 'cpp', vim.bo.filetype)

local function client()
  local get = vim.lsp.get_clients or vim.lsp.get_active_clients
  return get({ bufnr = 0, name = 'mcppls' })[1]
end
check('mcppls attached', vim.wait(30000, function() return client() ~= nil end, 100))
check('commands defined', vim.fn.exists(':McpplsStatus') == 2 and vim.fn.exists(':McpplsRestart') == 2)

local settled = vim.wait(180000, function()
  local s = mcppls.status_data(0)
  return s ~= nil and (s.state == 'ready' or s.state == 'degraded' or s.state == 'error')
end, 200)
local status = mcppls.status_data(0)
check('cxxModules/status received', settled, status and status.state)
check('the project is ready', status ~= nil and status.state == 'ready', mcppls.status(0))
check('statusline text', mcppls.status(0):find('^mcppls ready') ~= nil, mcppls.status(0))

-- A request retried until its answer is acceptable, as the first answers can come before the
-- module engine has the project.
local function request(method, params, accept)
  local deadline = uv.now() + 90000
  local last
  repeat
    local responses = vim.lsp.buf_request_sync(0, method, params, 15000)
    for _, response in pairs(responses or {}) do
      if response.result ~= nil then
        last = response.result
      end
    end
    if last ~= nil and accept(last) then
      return true, last
    end
    vim.wait(1000)
  until uv.now() > deadline
  return false, last
end

local function at(line, text)
  local content = vim.api.nvim_buf_get_lines(0, line, line + 1, false)[1]
  return {
    textDocument = vim.lsp.util.make_text_document_params(0),
    position = { line = line, character = content:find(text, 1, true) - 1 },
  }
end

local function uris(result)
  local out = {}
  for _, location in ipairs(vim.islist and (vim.islist(result) and result or { result }) or result) do
    local uri = location.targetUri or location.uri
    if uri then
      out[#out + 1] = vim.uri_to_fname(uri)
    end
  end
  return out
end

local function any(list, suffix)
  for _, item in ipairs(list) do
    if item:sub(-#suffix) == suffix then
      return true
    end
  end
  return false
end

local ok, result = request('textDocument/definition', at(4, 'greet'), function(r) return any(uris(r), 'greet.cppm') end)
check('definition of hello::greet lands in greet.cppm', ok, vim.inspect(uris(result or {})))

ok, result = request('textDocument/definition', at(1, 'hello.greet'), function(r) return any(uris(r), 'greet.cppm') end)
check('definition of `import hello.greet;` lands in greet.cppm', ok, vim.inspect(uris(result or {})))

ok, result = request('textDocument/hover', at(4, 'greet'), function(r)
  return r.contents ~= nil and vim.inspect(r.contents):find('greet') ~= nil
end)
check('hover on hello::greet names it', ok)

-- The declaration's file is open, as it is in the VS Code suite: the engine answers references
-- from the files it has, and greet.cppm is not one of them until something opens it.
local main = vim.api.nvim_get_current_buf()
vim.cmd.edit('src' .. sep .. 'greet' .. sep .. 'greet.cppm')
vim.api.nvim_set_current_buf(main)
local references = at(4, 'greet')
references.context = { includeDeclaration = true }
ok, result = request('textDocument/references', references, function(r)
  local list = uris(r)
  return any(list, 'main.cpp') and any(list, 'greet.cppm')
end)
check('references of hello::greet span main.cpp and greet.cppm', ok, vim.inspect(uris(result or {})))

vim.api.nvim_buf_set_lines(0, 5, 5, false, { '    hello::' })
ok, result = request('textDocument/completion', {
  textDocument = vim.lsp.util.make_text_document_params(0),
  position = { line = 5, character = #'    hello::' },
}, function(r)
  for _, item in ipairs(r.items or r) do
    if (item.filterText or item.label):find('greet', 1, true) then
      return true
    end
  end
  return false
end)
check('completion after hello:: offers greet', ok)

check('nothing was shown to the user', #shown == 0, vim.inspect(shown))

-- initializationOptions.semanticTokens: `modules` follows the new `semantic_tokens_modules`
-- setup() option (default true); `moduleType` is always true, since this plugin knows the custom
-- `module` type (design .agents/docs/2026-09-25-import-hang-status-highlight.md §12, "Neovim").
-- config() is pure (it starts nothing), so these are cheap to check without touching the live
-- client; the reuse below only reattaches main.cpp to the same already-running server
-- (reuse_client_default in Neovim's own vim.lsp.start matches by name and root_dir, not
-- init_options).
local cfg = mcppls.config()
check('semanticTokens.modules defaults to true', cfg.init_options.semanticTokens.modules == true, vim.inspect(cfg.init_options))
check('moduleType is always sent as true', cfg.init_options.semanticTokens.moduleType == true, vim.inspect(cfg.init_options))

mcppls.setup({ server = server, semantic_tokens_modules = false })
cfg = mcppls.config()
check('semantic_tokens_modules = false is respected', cfg.init_options.semanticTokens.modules == false, vim.inspect(cfg.init_options.semanticTokens))

mcppls.setup({ server = server, semantic_tokens_modules = false, init_options = { semanticTokens = { modules = true, moduleType = false } } })
cfg = mcppls.config()
check("a user's own init_options.semanticTokens wins outright", cfg.init_options.semanticTokens.modules == true and cfg.init_options.semanticTokens.moduleType == false,
  vim.inspect(cfg.init_options.semanticTokens))
check("conflictArbitration stays forced even with a user's own init_options", cfg.init_options.conflictArbitration == 'client', cfg.init_options.conflictArbitration)

-- Back to the plugin's defaults for the checks below.
mcppls.setup({ server = server })
check('nothing was shown while checking init_options', #shown == 0, vim.inspect(shown))

-- `@lsp.type.module` and `@lsp.type.keyword` are `default` links, set at setup and put back on
-- every ColorScheme (colorschemes clear existing links when they load), so a colorscheme or the
-- user's own nvim_set_hl wins over them.
local function hl(name)
  return vim.api.nvim_get_hl(0, { name = name })
end
check('@lsp.type.module links to @module by default', hl('@lsp.type.module').link == '@module', vim.inspect(hl('@lsp.type.module')))
check('@lsp.type.keyword links to @keyword', hl('@lsp.type.keyword').link == '@keyword', vim.inspect(hl('@lsp.type.keyword')))

local user_color = tonumber('0x123456')
vim.api.nvim_set_hl(0, '@lsp.type.module', { fg = user_color })
vim.api.nvim_exec_autocmds('ColorScheme', { modeline = false })
check("a user's own @lsp.type.module survives the ColorScheme default being reapplied",
  hl('@lsp.type.module').fg == user_color, vim.inspect(hl('@lsp.type.module')))

-- A second C++ server on the same buffer: an in-process stand-in named clangd/ccls. `notify('exit')`
-- drives the dispatcher's on_exit the way a real server's exit would, so a full `client:stop()`
-- is observable the same way a real one is.
local function fake_server(dispatchers)
  local function exit()
    if dispatchers and dispatchers.on_exit then
      vim.schedule(function() dispatchers.on_exit(0, 0) end)
    end
  end
  return {
    request = function(method, _, callback)
      if method == 'initialize' then
        callback(nil, { capabilities = {} })
      else
        callback(nil, nil)
      end
      return true, 1
    end,
    notify = function(method)
      if method == 'exit' then exit() end
      return true
    end,
    is_closing = function() return false end,
    terminate = exit,
  }
end
local function stop_fake(c)
  if vim.fn.has('nvim-0.11') == 1 then c:stop(true) else c.stop(true) end
end

-- The plugin says so, once, and names it (disable_conflicting stays the default, false).
vim.lsp.start({ name = 'clangd', cmd = fake_server, root_dir = workspace }, { bufnr = 0 })
vim.wait(10000, function() return #shown > 0 end, 100)
check('a second C++ server is named, once', #shown == 1 and shown[1]:find('clangd', 1, true) ~= nil, vim.inspect(shown))
check('and it mentions disable_conflicting', shown[1] and shown[1]:find('disable_conflicting', 1, true) ~= nil, vim.inspect(shown))
vim.lsp.start({ name = 'clangd', cmd = fake_server, root_dir = workspace .. sep .. 'other' }, { bufnr = 0 })
vim.wait(2000)
check('and only once', #shown == 1, #shown)

-- Clear both stand-in `clangd` clients before the disable_conflicting checks below, so they do not
-- also get caught by the next LspAttach on this buffer.
local get = vim.lsp.get_clients or vim.lsp.get_active_clients
for _, c in ipairs(get({ name = 'clangd' })) do
  stop_fake(c)
end
vim.wait(5000, function() return #get({ name = 'clangd' }) == 0 end, 100)

-- disable_conflicting = true: the conflicting client is stopped for the buffer instead, and the
-- plugin still says so, once, naming it. A client that serves only buffers mcppls also serves is
-- stopped outright; one that serves another buffer too is only detached from this one (README).
local shown_before = #shown
mcppls.setup({ server = server, disable_conflicting = true })

vim.lsp.start({ name = 'ccls', cmd = fake_server, root_dir = workspace .. sep .. 'ccls-solo' }, { bufnr = 0 })
check('disable_conflicting stops a single-buffer conflicting client',
  vim.wait(10000, function() return #get({ name = 'ccls' }) == 0 end, 100),
  vim.inspect(get({ name = 'ccls' })))
check('and says so once, naming it', #shown == shown_before + 1 and shown[#shown]:find('ccls', 1, true) ~= nil, vim.inspect(shown))

local scratch = vim.api.nvim_create_buf(false, true)
vim.lsp.start({ name = 'ccls', cmd = fake_server, root_dir = workspace .. sep .. 'ccls-multi' }, { bufnr = scratch })
vim.lsp.start({ name = 'ccls', cmd = fake_server, root_dir = workspace .. sep .. 'ccls-multi' }, { bufnr = 0 })
vim.wait(5000, function() return #shown > shown_before + 1 end, 100)
check('disable_conflicting only detaches a client that also serves another buffer',
  vim.wait(5000, function() return #get({ bufnr = 0, name = 'ccls' }) == 0 end, 100) and #get({ bufnr = scratch, name = 'ccls' }) == 1,
  string.format('buf0 count=%d scratch count=%d', #get({ bufnr = 0, name = 'ccls' }), #get({ bufnr = scratch, name = 'ccls' })))
check('exactly one more notice, naming it', #shown == shown_before + 2 and shown[#shown]:find('ccls', 1, true) ~= nil, vim.inspect(shown))

local c = client()
if c then
  if vim.fn.has('nvim-0.11') == 1 then c:stop() else c.stop() end
  vim.wait(10000, function() return client() == nil end, 100)
end
vim.fn.delete(workspace, 'rf')
print(failures == 0 and 'all checks passed' or (failures .. ' check(s) failed'))
os.exit(failures == 0 and 0 or 1)
