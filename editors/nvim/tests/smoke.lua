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

-- A second C++ server on the same buffer: an in-process stand-in named clangd. The plugin says so,
-- once, and names it.
local function fake_server()
  return {
    request = function(method, _, callback)
      if method == 'initialize' then
        callback(nil, { capabilities = {} })
      else
        callback(nil, nil)
      end
      return true, 1
    end,
    notify = function() return true end,
    is_closing = function() return false end,
    terminate = function() end,
  }
end
vim.lsp.start({ name = 'clangd', cmd = fake_server, root_dir = workspace }, { bufnr = 0 })
vim.wait(10000, function() return #shown > 0 end, 100)
check('a second C++ server is named, once', #shown == 1 and shown[1]:find('clangd', 1, true) ~= nil, vim.inspect(shown))
vim.lsp.start({ name = 'clangd', cmd = fake_server, root_dir = workspace .. sep .. 'other' }, { bufnr = 0 })
vim.wait(2000)
check('and only once', #shown == 1, #shown)

local c = client()
if c then
  if vim.fn.has('nvim-0.11') == 1 then c:stop() else c.stop() end
  vim.wait(10000, function() return client() == nil end, 100)
end
vim.fn.delete(workspace, 'rf')
print(failures == 0 and 'all checks passed' or (failures .. ' check(s) failed'))
os.exit(failures == 0 and 0 or 1)
