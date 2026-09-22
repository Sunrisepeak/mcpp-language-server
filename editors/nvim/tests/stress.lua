-- Real-project stress testing (real-project plan RP0) through the real plugin: opens
-- files under a workspace at random, some in quick succession without waiting, and at random
-- identifier positions asks hover, definition, references, completion or documentSymbol. Records
-- per method answered/empty/timeout/error and p50/p90/max latency, the status timeline's longest
-- stall while not ready, and the final state. Exits non-zero when a budget env var is crossed, so
-- this is a CI check, not only a report.
--
--   MCPPLS_SERVER=<payload>/bin/mcppls nvim --headless --clean -l editors/nvim/tests/stress.lua [FIXTURE]
--
-- FIXTURE (default "module-faults") names a conformance/fixtures directory; its files are copied
-- to a private workspace, as smoke.lua does. Env vars, all optional:
--   ACTIONS (default 60), SEED (default 1), REQUEST_TIMEOUT_MS (default 10000)
--   STRESS_BUDGET_TIMEOUTS, STRESS_BUDGET_P90_MS, STRESS_BUDGET_MAX_STALL_MS: a budget crossed
--   fails the run (exit 1) instead of only being reported.
--   STRESS_OUT: a file the JSON summary is also written to.

local uv = vim.uv or vim.loop
local sep = package.config:sub(1, 1)
local here = vim.fn.fnamemodify(debug.getinfo(1, 'S').source:sub(2), ':p:h')
local plugin = vim.fn.fnamemodify(here, ':h')
local repo = vim.fn.fnamemodify(plugin, ':h:h')
local fixture = arg[1] or 'module-faults'

local server = os.getenv('MCPPLS_SERVER')
if not server or server == '' then
  io.stderr:write('MCPPLS_SERVER is not set\n')
  os.exit(2)
end

local actions = tonumber(os.getenv('ACTIONS') or '60')
local seed = tonumber(os.getenv('SEED') or '1')
local request_timeout_ms = tonumber(os.getenv('REQUEST_TIMEOUT_MS') or '10000')
math.randomseed(seed)

local t0 = uv.hrtime()
local function now_ms() return (uv.hrtime() - t0) / 1e6 end

-- A private copy, so nothing the server writes lands in the repository (smoke.lua's own).
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
local workspace = vim.fn.tempname() .. '-mcppls-nvim-stress'
copy(table.concat({ repo, 'conformance', 'fixtures', fixture }, sep), workspace)
vim.fn.chdir(workspace)

vim.opt.runtimepath:prepend(plugin)
local mcppls = require('mcppls')
mcppls.setup({ server = server })

-- The status timeline: every state this session ever saw, with when it arrived.
local timeline = {}
local last_state
local function poll_status()
  local data = mcppls.status_data(0)
  local state = data and data.state
  if state and state ~= last_state then
    last_state = state
    timeline[#timeline + 1] = { at = now_ms(), state = state }
  end
end

local function client()
  local get = vim.lsp.get_clients or vim.lsp.get_active_clients
  return get({ name = 'mcppls' })[1]
end

-- Every C++ source under the workspace: what a stress run picks files from.
local files = {}
local function collect(dir, relative)
  for name, kind in vim.fs.dir(dir) do
    local abs, rel = dir .. sep .. name, relative == '' and name or (relative .. '/' .. name)
    if kind == 'directory' then
      collect(abs, rel)
    elseif name:match('%.cppm$') or name:match('%.cpp$') then
      files[#files + 1] = rel
    end
  end
end
collect(workspace, '')
if #files == 0 then
  io.stderr:write('no .cppm/.cpp file under ' .. workspace .. '\n')
  os.exit(2)
end

local KEYWORDS = {
  const = 1, auto = 1, ['return'] = 1, ['if'] = 1, ['for'] = 1, ['while'] = 1, std = 1, int = 1,
  void = 1, bool = 1, class = 1, struct = 1, namespace = 1, import = 1, export = 1, module = 1,
  using = 1, public = 1, private = 1, case = 1, switch = 1, ['else'] = 1, ['do'] = 1, new = 1,
  delete = 1, this = 1, ['true'] = 1, ['false'] = 1, nullptr = 1, static = 1, constexpr = 1,
}

-- The plugin attaches on its own `FileType` autocommand (editors/nvim/lua/mcppls/init.lua), the
-- same as smoke.lua relies on: opening a buffer here is all a real editor's own "open a file" is.
local current_buf
local function open(relative)
  vim.cmd.edit(vim.fn.fnameescape(relative))
  current_buf = vim.api.nvim_get_current_buf()
  vim.bo[current_buf].filetype = 'cpp'
  return current_buf
end

local function random_identifier(buf)
  local lines = vim.api.nvim_buf_get_lines(buf, 0, -1, false)
  if #lines == 0 then return nil end
  for _ = 1, 50 do
    local i = math.random(1, #lines)
    local line = lines[i]
    if not line:match('^%s*//') and not line:match('^%s*#') then
      local spots = {}
      for s, word in line:gmatch('()([%a_][%w_]*)') do
        if #word > 2 and not KEYWORDS[word] then
          spots[#spots + 1] = { line = i - 1, character = s - 1 + math.floor(#word / 2), finish = s - 1 + #word }
        end
      end
      if #spots > 0 then return spots[math.random(1, #spots)] end
    end
  end
  return nil
end

local METHODS = { 'textDocument/hover', 'textDocument/definition', 'textDocument/references',
                  'textDocument/completion', 'textDocument/documentSymbol' }
local results = {}   -- method -> { answered, empty, timeout, error, lat = {ms...} }
local function record(method, outcome, took_ms)
  local r = results[method] or { answered = 0, empty = 0, timeout = 0, error = 0, lat = {} }
  results[method] = r
  r[outcome] = r[outcome] + 1
  if outcome == 'answered' or outcome == 'empty' then r.lat[#r.lat + 1] = took_ms end
end

local function is_empty(method, result)
  if result == nil then return true end
  if method == 'textDocument/hover' then
    return result.contents == nil or (type(result.contents) == 'string' and result.contents == '')
  end
  if method == 'textDocument/documentSymbol' or method == 'textDocument/references' then
    return type(result) == 'table' and vim.tbl_isempty(result)
  end
  if method == 'textDocument/completion' then
    local items = result.items or result
    return type(items) == 'table' and vim.tbl_isempty(items)
  end
  return type(result) == 'table' and vim.tbl_isempty(result)
end

local function ask(buf, method, params)
  poll_status()
  local started = now_ms()
  local responses = vim.lsp.buf_request_sync(buf, method, params, request_timeout_ms)
  poll_status()
  local took = now_ms() - started
  local response
  for _, r in pairs(responses or {}) do response = r end
  local outcome
  if response == nil then
    outcome = 'timeout'
  elseif response.err ~= nil then
    outcome = 'error'
  elseif is_empty(method, response.result) then
    outcome = 'empty'
  else
    outcome = 'answered'
  end
  record(method, outcome, took)
end

-- Wait for the client to attach and settle once before stressing it (this is about steady-state
-- use, not cold start, which the conformance runner's own `stress` check already covers).
open(files[math.random(1, #files)])
vim.wait(60000, function()
  local data = mcppls.status_data(0)
  return client() ~= nil and data ~= nil and (data.state == 'ready' or data.state == 'degraded' or data.state == 'error')
end, 200)
poll_status()

local window_start = now_ms()
for _ = 1, actions do
  local roll = math.random()
  if roll < 0.25 then
    open(files[math.random(1, #files)])
  elseif roll < 0.30 then
    for _ = 1, 3 do open(files[math.random(1, #files)]) end   -- fast switching, no waiting
  end
  local spot = random_identifier(current_buf)
  if spot then
    local method = METHODS[math.random(1, #METHODS)]
    local params = {
      textDocument = vim.lsp.util.make_text_document_params(current_buf),
      position = { line = spot.line, character = spot.character },
    }
    if method == 'textDocument/references' then params.context = { includeDeclaration = true } end
    if method == 'textDocument/completion' then params.position.character = spot.finish end
    if method == 'textDocument/documentSymbol' then params = { textDocument = params.textDocument } end
    ask(current_buf, method, params)
  end
end
local window_end = now_ms()

-- The longest gap between consecutive timeline events while the state was not "ready".
local max_stall_ms = 0
do
  local marks = { window_start, window_end }
  for _, event in ipairs(timeline) do
    if event.at >= window_start and event.at <= window_end then marks[#marks + 1] = event.at end
  end
  table.sort(marks)
  local state = 'unknown'
  for _, event in ipairs(timeline) do
    if event.at <= window_start then state = event.state end
  end
  local ei = 1
  for i = 1, #marks - 1 do
    while ei <= #timeline and timeline[ei].at <= marks[i] do
      state = timeline[ei].state
      ei = ei + 1
    end
    if state ~= 'ready' then max_stall_ms = math.max(max_stall_ms, marks[i + 1] - marks[i]) end
  end
end

local function pct(values, p)
  if #values == 0 then return 0 end
  local sorted = vim.deepcopy(values)
  table.sort(sorted)
  return sorted[math.max(1, math.ceil(#sorted * p))]
end

local summary = { methods = {}, timeline = timeline, maxStallMs = max_stall_ms, finalState = last_state }
local total_timeouts, worst_p90 = 0, 0
print(string.format('%-16s %8s %6s %8s %6s %9s %9s %9s', 'method', 'answered', 'empty', 'timeout', 'error', 'p50(ms)', 'p90(ms)', 'max(ms)'))
for _, method in ipairs(METHODS) do
  local r = results[method]
  if r then
    local p50, p90, mx = pct(r.lat, 0.5), pct(r.lat, 0.9), pct(r.lat, 1)
    print(string.format('%-16s %8d %6d %8d %6d %9.1f %9.1f %9.1f', method:gsub('textDocument/', ''), r.answered, r.empty, r.timeout, r.error, p50, p90, mx))
    summary.methods[method] = { answered = r.answered, empty = r.empty, timeout = r.timeout, error = r.error, p50 = p50, p90 = p90, max = mx }
    total_timeouts = total_timeouts + r.timeout
    worst_p90 = math.max(worst_p90, p90)
  end
end
print(string.format('final state: %s, max stall: %.0fms, wall: %.0fms', tostring(last_state), max_stall_ms, window_end - window_start))

local out = os.getenv('STRESS_OUT')
if out then
  local f = io.open(out, 'w')
  if f then
    f:write(vim.json.encode(summary))
    f:close()
  end
end

local failures = 0
local function budget(name, actual, envName, unit)
  local limit = tonumber(os.getenv(envName) or '')
  if limit and actual > limit then
    print(string.format('FAIL budget %s: %.1f%s over %.1f%s (%s)', name, actual, unit, limit, unit, envName))
    failures = failures + 1
  end
end
budget('timeouts', total_timeouts, 'STRESS_BUDGET_TIMEOUTS', '')
budget('p90', worst_p90, 'STRESS_BUDGET_P90_MS', 'ms')
budget('max stall', max_stall_ms, 'STRESS_BUDGET_MAX_STALL_MS', 'ms')

local c = client()
if c then
  if vim.fn.has('nvim-0.11') == 1 then c:stop() else c.stop() end
  vim.wait(10000, function() return client() == nil end, 100)
end
vim.fn.delete(workspace, 'rf')
os.exit(failures == 0 and 0 or 1)
