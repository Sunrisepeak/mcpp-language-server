#!/usr/bin/env bash
# The components a change reaches, from .github/components.tsv (docs/92-release.md "Versions").
#
#   changes.sh <changed-paths-file>   prints `<component>=true|false`, `full=`, each flag, and `digest=`
#
# EVENT, BASE_REF and HEAD_REF (the workflow's event and a pull request's branches) decide a full run:
# anything but a pull request, and a pull request from or into a release branch, reaches every component.
# Otherwise each path reaches the component of the first line of the table that matches it; a path no
# line matches, or one of `all`, reaches every component; and a change of the `@core` component reaches
# every other, since each is tested over it. A `@flag` is a pull request's question: true when a changed
# path matches it, or when the pull request is from or into a release branch; false for any other event.
#
# `digest` names the core's inputs (the blobs of every path of the core and of `all`), so a run that built
# the core from the same inputs can lend what it built to a run that did not change them.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
table="$root/.github/components.tsv"
changed="${1:?the file of changed paths}"

rows=(); flags=(); core=""; components=()
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    case "$line" in
        @core$'\t'*) core="${line#*$'\t'}" ;;
        @flag\ *) flags+=("${line#@flag }") ;;
        *) rows+=("$line")
           name="${line%%$'\t'*}"
           [ "$name" = all ] || [[ " ${components[*]} " == *" $name "* ]] || components+=("$name") ;;
    esac
done < "$table"

declare -A reached=() flagged=() printed=()
full=false
release=false
case "${EVENT:-}" in pull_request) ;; *) full=true ;; esac
case "${BASE_REF:-}" in release/*) full=true; release=true ;; esac
case "${HEAD_REF:-}" in release/*) full=true; release=true ;; esac

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
    for flag in "${flags[@]}"; do
        name="${flag%%$'\t'*}"; pattern="${flag#*$'\t'}"
        # shellcheck disable=SC2254
        case "$path" in $pattern) flagged[$name]=true ;; esac
    done
done < "$changed"

if [ "$full" = true ] || { [ -n "$core" ] && [ "${reached[$core]:-}" = true ]; }; then
    for c in "${components[@]}"; do reached[$c]=true; done
fi
for c in "${components[@]}"; do echo "$c=${reached[$c]:-false}"; done
echo "full=$full"
for flag in "${flags[@]}"; do
    name="${flag%%$'\t'*}"
    [ "$release" = true ] && flagged[$name]=true
    [ "${EVENT:-}" = pull_request ] || flagged[$name]=false
    [ -n "${printed[$name]:-}" ] && continue
    printed[$name]=1
    echo "$name=${flagged[$name]:-false}"
done

# The core's inputs: every tracked path the table gives to the core or to `all`.
mapfile -t inputs < <(for row in "${rows[@]}"; do
    name="${row%%$'\t'*}"; pattern="${row#*$'\t'}"
    if [ "$name" = "$core" ] || [ "$name" = all ]; then echo "${pattern%%\**}"; fi
done | sort -u)
digest=$(cd "$root" && git ls-files -s -- "${inputs[@]}" | sha256sum | cut -c1-24)
echo "digest=$digest"
