#!/usr/bin/env bash
# The classification of .github/scripts/changes.sh, on lists of paths.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
failures=0
check() {   # check <description> <event> <expected key=value...> -- <paths...>
    local what="$1" event="$2"; shift 2
    local want=()
    while [ "$1" != -- ]; do want+=("$1"); shift; done
    shift
    local list; list=$(mktemp)
    printf '%s\n' "$@" > "$list"
    local got; got=$(EVENT="$event" BASE_REF=main HEAD_REF=topic bash "$here/changes.sh" "$list" 2>/dev/null)
    rm -f "$list"
    for kv in "${want[@]}"; do
        if ! grep -qx "$kv" <<<"$got"; then
            echo "FAIL: $what: wanted $kv, got: $(tr '\n' ' ' <<<"$got")"
            failures=$((failures + 1))
        fi
    done
}
check "a plugin alone" pull_request server=false clion=true vscode=false zed=false full=false -- editors/clion/src/main/kotlin/X.kt
check "its README is the plugin's, not documentation" pull_request vscode=true docs=false server=false -- editors/vscode/README.md
check "documentation alone" pull_request docs=true server=false vscode=false clion=false -- docs/92-release.md README.md
check "the server reaches every plugin" pull_request server=true vscode=true clion=true zed=true nvim=true agents=true -- src/server/session.cpp
check "a workflow reaches everything" pull_request full=true server=true -- .github/workflows/ci.yml
check "a path of no component reaches everything" pull_request full=true server=true -- newdir/thing.txt
check "a push reaches everything" push full=true server=true docs=true -- docs/x.md
check "the real Zed for the protocol" pull_request zed_real=true server=true -- src/lsp/protocol.cpp
check "release packaging" pull_request packaging=true -- packaging/release.manifest.json
check "packaging is a pull request's question" push packaging=false -- packaging/release.manifest.json
# The digest ignores what is not the server's.
list=$(mktemp); echo docs/x.md > "$list"
d1=$(EVENT=pull_request bash "$here/changes.sh" "$list" 2>/dev/null | sed -n 's/^digest=//p')
[ -n "$d1" ] && [ "${#d1}" -eq 24 ] || { echo "FAIL: no digest"; failures=$((failures + 1)); }
rm -f "$list"
[ "$failures" -eq 0 ] && echo "test_changes: every case held"
exit "$failures"
