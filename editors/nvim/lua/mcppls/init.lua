-- The Neovim side of mcpp-language-server: find the server, start it for C and C++ buffers, and
-- keep what its cxxModules/status notification says for the statusline.
--
-- Everything that makes mcppls worth using happens in the server, so this is small on purpose, like
-- the Zed extension. Two ways in, one configuration behind both:
--
--   require('mcppls').setup()     Neovim 0.10 and later: starts the server on C/C++ buffers
--   vim.lsp.enable('mcppls')      Neovim 0.11 and later: lsp/mcppls.lua, the same configuration

local M = {}

local uv = vim.uv or vim.loop
local is_windows = uv.os_uname().sysname:find('Windows') ~= nil

-- Module interface units Neovim may not know as C++ yet.
vim.filetype.add({
  extension = { cppm = 'cpp', ixx = 'cpp', mpp = 'cpp', ccm = 'cpp', cxxm = 'cpp', ['c++m'] = 'cpp' },
})

M.filetypes = { 'c', 'cpp' }
-- The files that mark a project's root, nearest first; with none, the current directory is the root.
M.root_markers = { 'mcpp.toml', 'CMakeLists.txt', 'compile_commands.json', '.git' }

local options = {}
-- The latest cxxModules/status per client, as the server sent it (docs/specs/s3-lsp-extensions.md).
local statuses = {}

local function executable(path)
  return path ~= nil and path ~= '' and vim.fn.executable(path) == 1
end

-- <user data>/mcppls/payload/bin/mcppls, where `mcpp run -p devtools -- extension --install` puts the
-- server, with the same user data directory as the Zed extension and the installer.
local function installed_server()
  local data
  if is_windows then
    data = os.getenv('LOCALAPPDATA')
  elseif uv.os_uname().sysname == 'Darwin' then
    local home = os.getenv('HOME')
    data = home and (home .. '/Library/Application Support')
  else
    data = os.getenv('XDG_DATA_HOME')
    if not data or data:sub(1, 1) ~= '/' then
      local home = os.getenv('HOME')
      data = home and (home .. '/.local/share')
    end
  end
  if not data then
    return nil
  end
  local sep = is_windows and '\\' or '/'
  return table.concat({ data, 'mcppls', 'payload', 'bin', is_windows and 'mcppls.exe' or 'mcppls' }, sep)
end

--- The mcppls executable: `server` from setup(), then PATH, then the installed payload.
--- @return string|nil
function M.server_path()
  if executable(options.server) then
    return vim.fn.exepath(options.server)
  end
  if executable('mcppls') then
    return vim.fn.exepath('mcppls')
  end
  local installed = installed_server()
  if executable(installed) then
    return installed
  end
  return nil
end

--- The root a buffer belongs to: the nearest directory with a root marker, else the current directory.
--- @param bufnr integer
--- @return string
function M.root(bufnr)
  local name = vim.api.nvim_buf_get_name(bufnr)
  local start = name ~= '' and vim.fs.dirname(name) or vim.fn.getcwd()
  local marker = vim.fs.find(M.root_markers, { path = start, upward = true })[1]
  return marker and vim.fs.dirname(marker) or vim.fn.getcwd()
end

local function on_status(_, params, ctx)
  if type(params) ~= 'table' then
    return
  end
  statuses[ctx.client_id] = params
  vim.api.nvim_exec_autocmds('User', { pattern = 'McpplsStatus', modeline = false, data = params })
end

--- The configuration both ways in share: vim.lsp.start() and lsp/mcppls.lua.
--- @return table
function M.config()
  local capabilities = vim.lsp.protocol.make_client_capabilities()
  -- The server sends cxxModules/status only to a client that says it reads it (S3).
  capabilities.experimental = vim.tbl_extend('force', capabilities.experimental or {},
    { cxxModules = { version = 1, status = true } })
  -- `modules`: ask the server for module-syntax semantic tokens (design
  -- .agents/docs/2026-09-25-import-hang-status-highlight.md §7, §12). `moduleType = true`: this
  -- plugin knows the custom `module` type and `partition` modifier (set_highlight_defaults below),
  -- so the server need not fall back to `namespace`.
  local modules = options.semantic_tokens_modules
  if modules == nil then
    modules = true
  end
  local defaults = { semanticTokens = { modules = modules, moduleType = true } }
  return {
    name = 'mcppls',
    cmd = { M.server_path() or 'mcppls', 'serve' },
    filetypes = M.filetypes,
    -- A function rather than root_markers, so a project with no marker still gets a root (the
    -- current directory), as it does through setup().
    root_dir = function(bufnr, on_dir)
      on_dir(M.root(bufnr))
    end,
    capabilities = capabilities,
    -- `defaults` first, so a user's own init_options.semanticTokens replaces it outright; then
    -- conflictArbitration is forced last, as before: this plugin tells the user about a second
    -- C/C++ server itself (watch_conflicts below), so the server need not say it on every start.
    init_options = vim.tbl_extend('force', defaults, options.init_options or {}, { conflictArbitration = 'client' }),
    handlers = { ['cxxModules/status'] = on_status },
  }
end

local function start(bufnr)
  if not vim.api.nvim_buf_is_valid(bufnr) or vim.bo[bufnr].buftype ~= '' then
    return
  end
  if not M.server_path() then
    if not M._warned then
      M._warned = true
      vim.notify('mcppls is not on PATH; see editors/nvim/README.md', vim.log.levels.WARN)
    end
    return
  end
  local config = M.config()
  config.root_dir = M.root(bufnr)
  config.filetypes = nil
  vim.lsp.start(config, { bufnr = bufnr })
end

local function clients()
  local get = vim.lsp.get_clients or vim.lsp.get_active_clients
  return get({ name = 'mcppls' })
end

-- Client methods take `self` from Neovim 0.11 on; 0.10 has only the dot form.
local method_calls = vim.fn.has('nvim-0.11') == 1

local function client_stop(client)
  if method_calls then client:stop() else client.stop() end
end

local function client_request(client, method, params, handler)
  if method_calls then client:request(method, params, handler) else client.request(method, params, handler) end
end

local function client_of(bufnr)
  if bufnr == nil or bufnr == 0 then
    bufnr = vim.api.nvim_get_current_buf()
  end
  for _, client in ipairs(clients()) do
    if client.attached_buffers[bufnr] then
      return client
    end
  end
  return nil
end

--- The statusline text for a buffer's root, e.g. "mcppls ready · mcpp L1"; "" when there is none.
--- @param bufnr? integer
--- @return string
function M.status(bufnr)
  local client = client_of(bufnr)
  if client then
    local s = statuses[client.id]
    if not s then
      return 'mcppls starting'
    end
    local text = 'mcppls ' .. (s.state or '?')
    if s.project and s.project.source then
      -- `tier` (S3-4-8, S3-4-9) is the README's L1..L4, how the project was described; `level`
      -- is S1's own document-conformance number and reads as the same thing to
      -- someone who does not know the difference, so it is never shown here.
      text = text .. ' · ' .. s.project.source .. (s.project.tier and (' L' .. s.project.tier) or '')
    end
    if s.progress and s.progress.total and s.progress.total > 0 and s.state == 'preparing' then
      text = text .. string.format(' %d/%d', s.progress.done or 0, s.progress.total)
    end
    if s.issues and #s.issues > 0 then
      text = text .. ' · ' .. #s.issues .. (#s.issues == 1 and ' issue' or ' issues')
    end
    return text
  end
  return ''
end

--- The latest cxxModules/status of the client attached to a buffer, as the server sent it.
--- @param bufnr? integer
--- @return table|nil
function M.status_data(bufnr)
  local client = client_of(bufnr)
  return client and statuses[client.id] or nil
end

local function show_status()
  local s = M.status_data(0)
  if not s then
    vim.notify(M.status(0) ~= '' and M.status(0) or 'mcppls is not attached to this buffer')
    return
  end
  local lines = { M.status(0) }
  if s.project then
    table.insert(lines, 'root: ' .. (s.project.root or '?'))
  end
  if s.profile then
    table.insert(lines, string.format('profile: %s %s (%s)', s.profile.kind or '?', s.profile.stdlib or '?', s.profile.target or '?'))
  end
  if s.engine then
    table.insert(lines, string.format('engine: %s %s', s.engine.name or '?', s.engine.version or ''))
  end
  for _, issue in ipairs(s.issues or {}) do
    table.insert(lines, string.format('issue [%s]: %s', issue.code or '?', issue.message or ''))
  end
  for _, notice in ipairs(s.notices or {}) do
    table.insert(lines, string.format('notice [%s]: %s', notice.code or '?', notice.message or ''))
  end
  vim.notify(table.concat(lines, '\n'))
end

local function restart()
  local buffers = {}
  for _, client in ipairs(clients()) do
    for bufnr in pairs(client.attached_buffers) do
      buffers[#buffers + 1] = bufnr
    end
    client_stop(client)
  end
  vim.defer_fn(function()
    for _, bufnr in ipairs(buffers) do
      start(bufnr)
    end
  end, 500)
end

-- The server reads the build description again (it does the same when an editor regains focus),
-- e.g. after running the build tool by hand.
local function reload()
  for _, client in ipairs(clients()) do
    client_request(client, 'workspace/executeCommand', { command = 'mcppls.reloadBuildDescription', arguments = {} }, function() end)
  end
end

-- Language servers that answer for C and C++ files themselves. mcppls drives its own clangd; a
-- second, editor-started one over the same files is two engines answering (docs/10-editors.md).
M.conflicting = { clangd = true, ccls = true }

local conflict_told = false
local conflict_stopped = {}   -- client id -> true, once disable_conflicting has dealt with it

--- Detach `client` from `bufnr`, or stop it outright when that is its only buffer, so no idle
--- server is left with nothing attached. The least surprising choice: a client also serving other
--- buffers keeps running for them; one serving only buffers mcppls also serves is not worth
--- leaving alive (README "Options").
local function stop_conflicting(client, bufnr)
  if vim.tbl_count(client.attached_buffers or {}) <= 1 then
    client_stop(client)
  else
    vim.lsp.buf_detach_client(bufnr, client.id)
  end
end

local function watch_conflicts(group)
  vim.api.nvim_create_autocmd('LspAttach', {
    group = group,
    callback = function(args)
      if options.detect_conflicts == false then
        return
      end
      local get = vim.lsp.get_clients or vim.lsp.get_active_clients
      local mcppls_here = false
      local conflicting = {}
      for _, c in ipairs(get({ bufnr = args.buf })) do
        if c.name == 'mcppls' then
          mcppls_here = true
        elseif M.conflicting[c.name] then
          conflicting[#conflicting + 1] = c
        end
      end
      if not mcppls_here then
        return
      end
      for _, c in ipairs(conflicting) do
        if options.disable_conflicting then
          if not conflict_stopped[c.id] then
            conflict_stopped[c.id] = true
            local bufnr, name = args.buf, c.name
            -- Deferred to the next tick: Neovim's own Client:on_attach() sets
            -- attached_buffers[bufnr] again right after firing this same LspAttach, so detaching
            -- synchronously here would be undone as soon as this callback returns.
            vim.schedule(function()
              stop_conflicting(c, bufnr)
              vim.notify(string.format('mcppls already serves this buffer; stopped %s for it.', name), vim.log.levels.WARN)
            end)
          end
        elseif not conflict_told then
          conflict_told = true
          vim.notify(string.format('mcppls runs its own clangd; %s is also attached to this buffer, so two engines answer. '
            .. 'Stop starting %s for C and C++ (e.g. vim.lsp.enable(%q, false)), or set disable_conflicting = true '
            .. 'to have mcppls stop it itself.', c.name, c.name, c.name), vim.log.levels.WARN)
        end
      end
    end,
  })
end

-- `@lsp.type.module` (module and partition names) and `@lsp.type.keyword` (`import`, `module`,
-- `export`) as `default` links, so a colorscheme or the user's own nvim_set_hl still wins
-- (design .agents/docs/2026-09-25-import-hang-status-highlight.md §7, §12). Neovim already links
-- `@lsp.type.keyword` to `@keyword` on 0.10 through 0.12 (checked on 0.10.4 and 0.12.5); this is a
-- safety net in case some version, or a colorscheme's `hi clear`, ever leaves it unset.
local function set_highlight_defaults()
  vim.api.nvim_set_hl(0, '@lsp.type.module', { link = '@module', default = true })
  vim.api.nvim_set_hl(0, '@lsp.type.keyword', { link = '@keyword', default = true })
end

local commands_defined = false
local function define_commands()
  if commands_defined then
    return
  end
  commands_defined = true
  watch_conflicts(vim.api.nvim_create_augroup('mcppls-conflicts', { clear = true }))
  set_highlight_defaults()
  -- Colorschemes clear existing links when they load, so put the defaults back each time.
  vim.api.nvim_create_autocmd('ColorScheme', {
    group = vim.api.nvim_create_augroup('mcppls-highlights', { clear = true }),
    callback = set_highlight_defaults,
  })
  vim.api.nvim_create_user_command('McpplsStatus', show_status, { desc = 'mcppls: the build description, engine and issues' })
  vim.api.nvim_create_user_command('McpplsRestart', restart, { desc = 'mcppls: restart the server' })
  vim.api.nvim_create_user_command('McpplsReload', reload, { desc = 'mcppls: read the build description again' })
end

--- Start mcppls on C and C++ buffers.
--- @param opts? { server?: string, init_options?: table, filetypes?: string[], root_markers?: string[], detect_conflicts?: boolean, semantic_tokens_modules?: boolean, disable_conflicting?: boolean }
function M.setup(opts)
  opts = opts or {}
  options.server = opts.server
  options.init_options = opts.init_options
  options.detect_conflicts = opts.detect_conflicts
  options.semantic_tokens_modules = opts.semantic_tokens_modules
  options.disable_conflicting = opts.disable_conflicting
  if opts.filetypes then
    M.filetypes = opts.filetypes
  end
  if opts.root_markers then
    M.root_markers = opts.root_markers
  end
  define_commands()
  local group = vim.api.nvim_create_augroup('mcppls', { clear = true })
  vim.api.nvim_create_autocmd('FileType', {
    group = group,
    pattern = M.filetypes,
    callback = function(args)
      start(args.buf)
    end,
  })
  -- Buffers already open when setup() runs, e.g. from a lazy-loaded plugin manager.
  for _, bufnr in ipairs(vim.api.nvim_list_bufs()) do
    if vim.api.nvim_buf_is_loaded(bufnr) and vim.tbl_contains(M.filetypes, vim.bo[bufnr].filetype) then
      start(bufnr)
    end
  end
end

-- Commands exist for vim.lsp.enable('mcppls') too, where setup() never runs.
M._define_commands = define_commands

return M
