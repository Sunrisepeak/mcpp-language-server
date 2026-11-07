#!/usr/bin/env bash
# The components a change reaches (docs/92-release.md "Versions"), from .github/components.tsv.
#
#   changes.sh <changed-paths-file>     prints `<name>=true|false` lines, and `digest=<hex>`
#
# EVENT, BASE_REF and HEAD_REF (the workflow's event name and a pull request's branches) decide a
# full run: anything but a pull request, and a pull request from or into a release branch, reaches
# every component. Otherwise each path reaches the component of the first line of the table that
# matches it, and a path no line matches, or one of `all`, reaches every component. A change of the
# server reaches every plugin too, since each is tested over the payload built from it.
#
# `digest` names the server's inputs (the blobs of every path of `server` and `all`), so a run that
# built the payload from the same inputs can lend it to a run that did not change them.
#
# `zed_real` and `packaging` keep two narrower questions CI asked before: whether a pull request
# needs the real Zed, and whether it changes release packaging (release-checks.yml runs then).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
table="$root/.github/components.tsv"
changed="${1:?the file of changed paths}"
components=(server vscode clion zed nvim agents docs)

declare -A reached=()
full=false
case "${EVENT:-}" in pull_request) ;; *) full=true ;; esac
case "${BASE_REF:-}" in release/*) full=true ;; esac
case "${HEAD_REF:-}" in release/*) full=true ;; esac

mapfile -t rows < <(grep -v '^#' "$table" | grep -v '^[[:space:]]*$')
zed_real=false
packaging=false
while IFS= read -r path; do
    [ -n "$path" ] || continue
    component=""
    for row in "${rows[@]}"; do
        name="${row%%$'\t'*}"; pattern="${row#*$'\t'}"
        # shellcheck disable=SC2254
        case "$path" in $pattern) component="$name"; break ;; esac
    done
    if [ -z "$component" ] || [ "$component" = all ]; then
        echo "every component: $path" >&2
        full=true
    else
        reached[$component]=true
    fi
    case "$path" in
        editors/zed/*|src/lsp/*|src/orchestrator/*|tools/devtools/*|.github/workflows/zed-smoke.yml|.github/versions.env) zed_real=true ;;
    esac
    case "$path" in
        .github/workflows/release.yml|.github/workflows/prerelease.yml|.github/workflows/release-checks.yml|\
        packaging/release.manifest.json|packaging/payload.lock.json|packaging/xlings/*|editors/zed/*|editors/clion/*|\
        modules/pack/src/*|tools/devtools/src/editors.*|tools/devtools/src/packaging.*|tools/devtools/src/release.*|\
        tools/devtools/src/kit.*) packaging=true ;;
    esac
done < "$changed"

if [ "$full" = true ] || [ "${reached[server]:-}" = true ]; then
    for c in "${components[@]}"; do reached[$c]=true; done
fi
[ "$full" = true ] && zed_real=true
for c in "${components[@]}"; do echo "$c=${reached[$c]:-false}"; done
echo "full=$full"
echo "zed_real=$zed_real"
# Release packaging is asked of a pull request only: a push, a dispatch and a release run it anyway.
[ "${EVENT:-}" = pull_request ] || packaging=false
echo "packaging=$packaging"

# The server's inputs: every tracked path the table gives to `server` or `all`.
mapfile -t inputs < <(for row in "${rows[@]}"; do
    name="${row%%$'\t'*}"; pattern="${row#*$'\t'}"
    case "$name" in server|all) echo "${pattern%%\**}" ;; esac
done | sort -u)
digest=$(cd "$root" && git ls-files -s -- "${inputs[@]}" | sha256sum | cut -c1-24)
echo "digest=$digest"
