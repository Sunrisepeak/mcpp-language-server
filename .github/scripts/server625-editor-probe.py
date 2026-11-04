"""Collect bounded first-navigation evidence from the unchanged server625 payload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('mode', choices=('prepare', 'collect'))
a = ap.parse_args()
out = Path(os.environ['PROBE_OUT']).resolve()
out.mkdir(parents=True, exist_ok=True)
if a.mode == 'prepare':
    source = Path('editors/nvim/tests/smoke.lua')
    text = source.read_text()
    needle = "check('definition of hello::greet lands in greet.cppm', ok, vim.inspect(uris(result or {})))"
    assert text.count(needle) == 1
    observer = r'''
-- This diagnostic ends after the original first-navigation check. Its return
-- code and answer are evidence; it is not a replacement for the full smoke CI.
local report = vim.lsp.buf_request_sync(0, 'cxxModules/report', {}, 5000)
local log = vim.lsp.log and vim.lsp.log.get_filename and vim.lsp.log.get_filename()
  or (vim.lsp.get_log_path and vim.lsp.get_log_path())
local evidence = { kind = 'first-navigation-only', check_ok = ok,
  answer = result or vim.NIL, report = report or vim.NIL,
  status = mcppls.status_data(0) or vim.NIL, cwd = vim.fn.getcwd(),
  workspace = workspace, lsp_log = log or vim.NIL }
vim.fn.writefile({vim.json.encode(evidence)}, os.getenv('PROBE_OUT') .. '/neovim-evidence.json')
if log then
  local f = io.open(log, 'rb')
  if f then
    local n = f:seek('end') or 0
    f:seek('set', math.max(0, n - 8 * 1024 * 1024))
    local text = f:read('*a'); f:close()
    local dest = io.open(os.getenv('PROBE_OUT') .. '/neovim-lsp-tail.log', 'wb')
    if dest then dest:write(text); dest:close() end
  end
end
if ok then vim.cmd('qa!') else vim.cmd('cquit 1') end
'''
    text = text.replace('local uv = vim.uv or vim.loop', "vim.lsp.set_log_level('debug')\nlocal uv = vim.uv or vim.loop", 1)
    source.write_text(text.replace(needle, needle + '\n' + observer))
else:
    candidates = set()
    for p in out.glob('*.json'):
        try:
            data = json.loads(p.read_text())
        except (ValueError, OSError):
            continue
        pending = [data]
        while pending:
            value = pending.pop()
            if isinstance(value, dict):
                pending.extend(value.values())
            elif isinstance(value, list):
                pending.extend(value)
            elif isinstance(value, str) and value.endswith('.log'):
                candidates.add(Path(value))
    roots = [Path.home() / '.cache/mcppls', Path.home() / 'Library/Caches/mcppls']
    if os.environ.get('LOCALAPPDATA'):
        roots.append(Path(os.environ['LOCALAPPDATA']) / 'mcppls')
    roots += [out / 'cache']
    for root in roots:
        if root.is_dir():
            candidates.update(root.rglob('*.log'))
    logs = out / 'server-logs'
    logs.mkdir(exist_ok=True)
    receipts = []
    for p in sorted(candidates)[:64]:
        if not p.is_file() or p.is_symlink():
            continue
        size = p.stat().st_size
        with p.open('rb') as stream:
            stream.seek(max(0, size - 1024 * 1024))
            data = stream.read()
        name = hashlib.sha256(str(p).encode()).hexdigest()[:12] + '-' + p.name
        (logs / name).write_bytes(data)
        receipts.append(dict(source=str(p), bytes=size, preserved_tail=len(data), dest=name))
    (out / 'log-receipts.json').write_text(json.dumps(receipts, indent=2))
    for p in [Path('payload/payload.json'), *Path('payload/clangd').glob('*/engine.json')]:
        if p.is_file():
            dest = out / ('identity-' + p.parent.name + '-' + p.name)
            shutil.copy2(p, dest)
