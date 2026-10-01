#!/usr/bin/env bash
# The Zed extension in a real Zed (plan 0.0.8 part 2, Z-2 and Z-3).
#
#   ZED_EDITOR=<zed>/libexec/zed-editor MCPPLS_PAYLOAD=<payload> MCPPLS_DEVTOOLS=<mcppls-devtools> \
#       xvfb-run -a editors/zed/tests/smoke.sh recommended|default
#
# Linux only, and it needs a display: CI runs it under xvfb-run with mesa's lavapipe for Vulkan.
# Needs editors/zed/extension.wasm (`mcpp run -p devtools -- extension --editor zed`).
#
# Zed runs with a HOME, XDG directories and a server cache of its own under one scratch directory,
# so neither the user's Zed nor ~/.cache/mcppls is touched. The extension is installed the way
# release-checks installs it (`extension --editor zed --install --link`), and Zed is given a
# project: a copy of the `inferred` conformance fixture (C++ modules, no build system, no
# compiler), with its first module open.
#
# Zed's extension API cannot pass arguments, and mcppls reads its log level from `--log-level`
# only, so the server is started through a `mcppls` script first on PATH (which is the extension's
# first choice anyway). The script runs the real server with `--log-level debug` and keeps a copy
# of the LSP conversation in both directions, which is what is asserted: it is exactly what Zed
# sent and exactly what Zed received.
#
#   recommended   `"language_servers": ["mcppls", "!clangd"]` in the user's settings (Z-2): within
#                 60 s mcppls has been initialized by "Zed", been sent the file and published
#                 diagnostics for it, and Zed started no clangd of its own
#   default       Zed's own settings (Z-3): mcppls answers all the same, and Zed's clangd is started
#                 beside it; that both ran is recorded, because it is what the README tells users
#                 to turn off
#
# Both end by quitting Zed and asserting that no mcppls process is left.
#
# Environment:
#   ZED_EDITOR, MCPPLS_PAYLOAD, MCPPLS_DEVTOOLS   required
#   ZED_SMOKE_DIR        scratch directory (default: a new temporary one, removed on success)
#   ZED_SMOKE_BUDGET     seconds for mcppls to answer (default 60)
#   ZED_SMOKE_REQUIRE_CLANGD=1   `default` needs Zed's clangd running, not just started: Zed
#                                downloads it from GitHub, which a machine without network cannot do
set -euo pipefail

mode=${1:-recommended}
case "$mode" in recommended|default) ;; *) echo "usage: $0 recommended|default" >&2; exit 2 ;; esac
for name in ZED_EDITOR MCPPLS_PAYLOAD MCPPLS_DEVTOOLS; do
  if [ -z "${!name:-}" ]; then echo "$name is not set" >&2; exit 2; fi
done

here=$(cd "$(dirname "$0")" && pwd)
extension=$(dirname "$here")
repo=$(cd "$extension/../.." && pwd)
editor=$(readlink -f "$ZED_EDITOR")
payload=$(readlink -f "$MCPPLS_PAYLOAD")
devtools=$(readlink -f "$MCPPLS_DEVTOOLS")
file=src/greet/greet.cppm
budget=${ZED_SMOKE_BUDGET:-60}

if [ ! -f "$extension/extension.wasm" ]; then
  echo "$extension/extension.wasm is missing: mcpp run -p devtools -- extension --editor zed" >&2
  exit 2
fi

keep=false
if [ -n "${ZED_SMOKE_DIR:-}" ]; then keep=true; fi
root=$(readlink -m "${ZED_SMOKE_DIR:-$(mktemp -d)}")
rm -rf "$root"
mkdir -p "$root/home/.config/zed" "$root/home/.local/share" "$root/home/.cache" "$root/run" "$root/bin" "$root/cache"
chmod 700 "$root/run"
data="$root/home/.local/share"
zed_data="$data/zed"
# Zed listens on a unix socket in its data directory, and a socket path is at most 107 bytes; past
# that Zed says "zed is already running" and exits.
if [ "${#zed_data}" -gt 80 ]; then echo "$root is too deep: Zed's socket path would not fit; use a shorter ZED_SMOKE_DIR" >&2; exit 2; fi
workspace="$root/workspace"
server="$root/server/bin/mcppls"
cp -r "$repo/conformance/fixtures/inferred" "$workspace"

zed_pid=''
started=$SECONDS

say() { echo "[$((SECONDS - started)) s] $*"; }

summary() {
  say "$*"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then echo "- $*" >> "$GITHUB_STEP_SUMMARY"; fi
}

# The pids of the running mcppls server (the real one, not the script around it).
server_pids() {
  local exe
  for exe in /proc/[0-9]*/exe; do
    if [ "$(readlink "$exe" 2>/dev/null)" = "$server" ]; then basename "$(dirname "$exe")"; fi
  done
}

# A clangd that Zed started: a child of Zed, or a program out of the languages directory it
# downloads servers into. The clangd mcppls drives is a child of mcppls, and is neither.
zed_clangd_pid() {
  local found exe target
  found=$(ps -eo pid=,ppid=,args= | awk -v zed="$zed_pid" '$2 == zed && /clangd/ { print $1; exit }')
  if [ -n "$found" ]; then echo "$found"; return 0; fi
  for exe in /proc/[0-9]*/exe; do
    target=$(readlink "$exe" 2>/dev/null) || continue
    case "$target" in "$zed_data"/languages/*clangd*) basename "$(dirname "$exe")"; return 0 ;; esac
  done
  return 1
}

# Zed.log says so when it looks for, downloads or starts a language server. (Not just any mention
# of clangd: Zed logs the PATH it was given, and a runner's PATH may name it.)
zed_log_mentions_clangd() {
  grep -aqiE '(found user-installed language server|starting language server process|downloading github artifact).*clangd|language server "clangd"' \
      "$zed_data/logs/Zed.log" 2>/dev/null
}

facts() { python3 "$here/lsp_frames.py" "$1" 2>/dev/null || true; }
zed_said() { facts "$root/lsp-in.log"; }
mcppls_said() { facts "$root/lsp-out.log"; }

dump() {
  echo "::group::what was left behind"
  echo "--- mcppls started by Zed"; cat "$root/wrapper.log" 2>/dev/null || true
  echo "--- Zed said"; zed_said | head -20
  echo "--- mcppls said"; mcppls_said | head -20
  echo "--- Zed.log"; tail -n 60 "$zed_data/logs/Zed.log" 2>/dev/null || true
  echo "--- Zed's output"; tail -n 20 "$root/zed.out" 2>/dev/null || true
  echo "--- mcppls log"; tail -n 40 "$root"/cache/logs/server-*.log 2>/dev/null || true
  echo "::endgroup::"
}

failed=true
cleanup() {
  local pid
  if [ -n "$zed_pid" ] && kill -0 "$zed_pid" 2>/dev/null; then kill -9 "$zed_pid" 2>/dev/null || true; fi
  for pid in $(server_pids); do kill -9 "$pid" 2>/dev/null || true; done
  if $failed; then dump; echo "scratch directory kept: $root"; return; fi
  if ! $keep; then rm -rf "$root"; fi
}
trap cleanup EXIT

fail() { echo "FAIL $*" >&2; exit 1; }

# One deadline for what mcppls must have done, counted from the moment Zed was started.
wait_for() {
  local what=$1; shift
  until "$@"; do
    if ! kill -0 "$zed_pid" 2>/dev/null; then fail "Zed exited before: $what"; fi
    if (( SECONDS - started >= budget )); then fail "not within $budget s: $what"; fi
    sleep 1
  done
  say "ok   $what"
}

has_client()      { zed_said | grep -qx 'client Zed'; }
has_didopen()     { zed_said | grep -q "^didopen .*/$file\$"; }
has_diagnostics() { mcppls_said | grep -q "^diagnostics .*/$file "; }

# ---------------------------------------------------------------------------------------------
# The extension, installed the way a release is checked: linked into Zed's data directory, the
# payload put where the extension looks for it.
"$devtools" extension --editor zed --install --link --plugin "$extension" --payload "$payload" \
    --server-dir "$root/server" --zed-dir "$zed_data"
test -x "$server" || fail "$server was not installed"
test -e "$zed_data/extensions/installed/mcppls/extension.toml" || fail "the extension is not linked into Zed"

{
  echo '#!/usr/bin/env bash'
  printf 'echo "$$ $*" >> %q\n' "$root/wrapper.log"
  printf 'tee -a %q | %q "$@" --log-level debug | tee -a %q\n' "$root/lsp-in.log" "$server" "$root/lsp-out.log"
} > "$root/bin/mcppls"
chmod +x "$root/bin/mcppls"

case "$mode" in
  recommended)
    languages='"languages": { "C++": { "language_servers": ["mcppls", "!clangd"] } },' ;;
  default)
    languages='' ;;
esac
cat > "$root/home/.config/zed/settings.json" <<EOF
{
  $languages
  "telemetry": { "diagnostics": false, "metrics": false },
  "auto_update": false,
  "session": { "trust_all_worktrees": true }
}
EOF

# ---------------------------------------------------------------------------------------------
started=$SECONDS
say "starting Zed ($mode settings) on $workspace"
(
  unset WAYLAND_DISPLAY
  export HOME="$root/home" XDG_CONFIG_HOME="$root/home/.config" XDG_DATA_HOME="$data" \
         XDG_CACHE_HOME="$root/home/.cache" XDG_RUNTIME_DIR="$root/run" SHELL=/bin/bash \
         ZED_ALLOW_EMULATED_GPU=1 MCPPLS_CACHE_DIR="$root/cache" MCPPLS_LOG_LEVEL=debug \
         PATH="$root/bin:$PATH"
  exec "$editor" "$workspace/$file" "$workspace"
) > "$root/zed.out" 2>&1 &
zed_pid=$!

wait_for 'mcppls is initialized by a client named "Zed"' has_client
wait_for "mcppls is sent $file (didOpen)" has_didopen
wait_for "mcppls publishes diagnostics for $file" has_diagnostics

# The server's own log, at the level the wrapper passed. Any of its log files (Zed may have started a second instance,
# which logs to a file of its own), and a moment for the lines to reach the disk (CI run 36787135201 looked too soon).
has_log()       { ls "$root"/cache/logs/server-*.log >/dev/null 2>&1; }
has_debug_log() { grep -qs '\[debug\]' "$root"/cache/logs/server-*.log; }
wait_for "mcppls wrote its log under MCPPLS_CACHE_DIR" has_log
wait_for "the log is at debug level (--log-level debug reached the server)" has_debug_log

mcppls_pid=$(server_pids | head -1)
test -n "$mcppls_pid" || fail "no mcppls process while Zed is showing its answers"

case "$mode" in
  recommended)
    # Zed starts every server for a file when it opens it, and mcppls has answered, so a clangd
    # that was going to start has started. Give a slow one a moment more.
    sleep 5
    if clangd=$(zed_clangd_pid); then fail "Zed started its own clangd (pid $clangd) although the settings turn it off"; fi
    if zed_log_mentions_clangd; then fail "Zed.log mentions clangd although the settings turn it off"; fi
    summary "Zed ($mode settings): mcppls (pid $mcppls_pid) answered within $((SECONDS - started)) s; Zed started no clangd"
    ;;
  default)
    # Zed downloads clangd on first use (tens of megabytes from GitHub) before it starts it.
    clangd_deadline=$((SECONDS + 240))
    clangd=''
    until clangd=$(zed_clangd_pid); do
      if [ "${ZED_SMOKE_REQUIRE_CLANGD:-0}" != 1 ] && zed_log_mentions_clangd; then break; fi
      kill -0 "$zed_pid" 2>/dev/null || fail "Zed exited before it started clangd"
      (( SECONDS < clangd_deadline )) || fail "Zed did not start clangd beside mcppls within 240 s"
      sleep 2
    done
    if [ -n "$clangd" ]; then
      summary "Zed ($mode settings): both servers ran: mcppls (pid $mcppls_pid, answered within $((SECONDS - started)) s) and Zed's clangd (pid $clangd)"
    else
      summary "Zed ($mode settings): mcppls (pid $mcppls_pid) answered; Zed tried to start its own clangd but could not run it (no network for its download?)"
    fi
    kill -0 "$mcppls_pid" 2>/dev/null || fail "mcppls died once clangd started"
    ;;
esac

# ---------------------------------------------------------------------------------------------
say "quitting Zed"
kill "$zed_pid"
gone=false
for _ in $(seq 1 30); do
  if ! kill -0 "$zed_pid" 2>/dev/null && [ -z "$(server_pids)" ]; then gone=true; break; fi
  sleep 1
done
$gone || fail "after Zed quit, still running: Zed $(kill -0 "$zed_pid" 2>/dev/null && echo yes || echo no), mcppls [$(server_pids | tr '\n' ' ')]"
wait "$zed_pid" 2>/dev/null || true
zed_pid=''
summary "Zed ($mode settings): mcppls exited with Zed"

failed=false
say "PASS ($mode)"
