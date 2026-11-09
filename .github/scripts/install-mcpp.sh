#!/usr/bin/env bash
# Installs the pinned mcpp through xlings and selects the pinned LLVM toolchain.
# MCPP_VERSION=latest installs the newest mcpp the index has (nightly, usable plan 5.2). A branch of
# mcpp under review (MCPP_SOURCE_REF) is built by this one (.github/actions/setup-mcpp).
# Retries because the index and the release mirrors are reached over the network.
set -euo pipefail
: "${MCPP_VERSION:?pin the mcpp version in the workflow}"
: "${LLVM_VERSION:?pin the LLVM toolchain version in the workflow}"

package="mcpp@$MCPP_VERSION"
[ "$MCPP_VERSION" = latest ] && package=mcpp
# xlings by the configured mirror (GLOBAL), then by CN (GitCode first), each attempt with a refreshed index
# (a cached index can predate the pin, and xlings keeps it when an anonymous refresh fails); else the
# release itself, by mcpp's install.sh (GitHub, then GitCode, sha256 checked).
installed=no
for attempt in 1 2 3 4; do
    [ "$attempt" = 3 ] && { xlings config --mirror CN > /dev/null 2>&1 || true; }
    xlings update > /dev/null 2>&1 || true
    if xlings install "$package" -y -g; then installed=yes; break; fi
    sleep 20
done
xlings config --mirror GLOBAL > /dev/null 2>&1 || true
if [ "$installed" = no ]; then
    [ "$MCPP_VERSION" != latest ] || { echo "::error::$package could not be installed"; exit 1; }
    echo "::warning::xlings could not install $package; taking the release itself"
    prefix="${RUNNER_TEMP:-$HOME}/mcpp-release-$MCPP_VERSION"
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            # Windows: the release's zip, by the web address, else the API's.
            zip="mcpp-$MCPP_VERSION-windows-x86_64.zip"
            mkdir -p "$prefix"
            if ! curl -fsSL --retry 5 --retry-all-errors -o "$prefix/$zip" \
                   "https://github.com/mcpp-community/mcpp/releases/download/v$MCPP_VERSION/$zip"; then
                id=$(curl -fsSL --retry 3 --retry-all-errors -H "Authorization: Bearer ${GH_TOKEN:-${GITHUB_TOKEN:-}}" \
                       "https://api.github.com/repos/mcpp-community/mcpp/releases/tags/v$MCPP_VERSION" \
                     | python -c 'import json,sys; print(next(str(a["id"]) for a in json.load(sys.stdin)["assets"] if a["name"] == sys.argv[1]))' "$zip")
                curl -fsSL --retry 5 --retry-all-errors -H "Authorization: Bearer ${GH_TOKEN:-${GITHUB_TOKEN:-}}" \
                     -H "Accept: application/octet-stream" -o "$prefix/$zip" \
                     "https://api.github.com/repos/mcpp-community/mcpp/releases/assets/$id"
            fi
            (cd "$prefix" && unzip -qo "$zip" && rm -f "$zip")
            bindir=$(dirname "$(find "$prefix" -type f -name mcpp.exe -path '*/bin/*' | head -1)")
            prefix="${bindir%/bin}" ;;
        *)
            curl -fsSL --retry 3 --retry-all-errors https://raw.githubusercontent.com/mcpp-community/mcpp/main/install.sh \
                | MCPP_VERSION="$MCPP_VERSION" MCPP_PREFIX="$prefix" MCPP_NO_PATH=1 bash ;;
    esac
    export PATH="$prefix/bin:$PATH"
    [ -z "${GITHUB_PATH:-}" ] || echo "$prefix/bin" >> "$GITHUB_PATH"
fi
mcpp --version
mcpp self config --mirror GLOBAL

for attempt in 1 2 3; do
    if mcpp toolchain install llvm "$LLVM_VERSION"; then break; fi
    if [ "$attempt" = 3 ]; then exit 1; fi
    sleep 30
done
mcpp toolchain default "llvm@$LLVM_VERSION"
