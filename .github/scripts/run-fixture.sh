#!/usr/bin/env bash
# run-fixture.sh FIXTURE[@VARIANT] [ARGS...] -- one conformance fixture against the payload in
# ./payload with the runner in ./stage, as CI's conformance job runs it (conformance/README.md).
# mtest runs one such command per fixture (mtest.toml); ARGS go to mcppls-conformance.
#
# NAME@polling runs fixture NAME as a client that declares no dynamic registration of watched files,
# NAME@plain as one that declares no `experimental.cxxModules`, NAME@vscode, @neovim and @zed with
# the capabilities that editor sends. cmake-clang-bdb takes CMake 4.4.2 first on PATH, and
# cmake-msvc-bdb and cmake-clang-cl its developer environment ($RUNNER_TEMP/cmake-4.4.2,
# $RUNNER_TEMP/msvc-bdb.env). CONFORMANCE_MEASURES names a directory for each fixture's measures.
set -euo pipefail
fixture=$1; shift
exe=''; extra=()
if [ "${RUNNER_OS:-}" = Windows ]; then exe='.exe'; extra=(--msvc-env "$RUNNER_TEMP/msvc.env"); fi
path_prefix=''
case "$fixture" in
  cmake-clang-bdb) path_prefix="$RUNNER_TEMP/cmake-4.4.2/bin:" ;;
  cmake-msvc-bdb|cmake-clang-cl) extra=(--msvc-env "$RUNNER_TEMP/msvc-bdb.env") ;;
  # The real producer's first databases and server log, kept apart from a diagnostic run.
  mcpp-target-windows-real)
    evidence="$RUNNER_TEMP/mcpp-target-windows-real-first"
    case " $* " in *" --verbose "*) evidence="$RUNNER_TEMP/mcpp-target-windows-real-diagnostic" ;; esac
    extra+=(--workspace-dir "$evidence/workspace" --cache-dir "$evidence/cache") ;;
  watch-polling|*@polling) extra+=(--no-dynamic-watch) ;;
  *@plain) extra+=(--plain-client) ;;
  *@vscode) extra+=(--client vscode) ;;
  *@neovim) extra+=(--client neovim) ;;
  *@zed) extra+=(--client zed) ;;
esac
if [ -n "${CONFORMANCE_MEASURES:-}" ]; then
  mkdir -p "$CONFORMANCE_MEASURES"
  extra+=(--measure "$CONFORMANCE_MEASURES/${fixture//@/-}.json")
fi
PATH="$path_prefix$PATH" exec "stage/mcppls-conformance$exe" run --server payload/bin/mcppls.okb --payload payload \
  --keep-bundles "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/bundles" ${extra[@]+"${extra[@]}"} \
  --fixture "conformance/fixtures/${fixture%@*}" --timeout "${CONFORMANCE_TIMEOUT:-180}" "$@"
