#!/usr/bin/env bash
# Installs the pinned mcpp through xlings and selects the pinned LLVM toolchain.
# MCPP_VERSION=latest installs the newest mcpp the index has (nightly, usable plan 5.2).
# Retries because the index and the release mirrors are reached over the network.
set -euo pipefail
: "${MCPP_VERSION:?pin the mcpp version in the workflow}"
: "${LLVM_VERSION:?pin the LLVM toolchain version in the workflow}"

package="mcpp@$MCPP_VERSION"
[ "$MCPP_VERSION" = latest ] && package=mcpp
for attempt in 1 2 3 4 5; do
    xlings update > /dev/null 2>&1 || true
    if xlings install "$package" -y -g; then break; fi
    if [ "$attempt" = 5 ]; then
        echo "::error::$package could not be installed"
        exit 1
    fi
    sleep 30
done
mcpp --version
mcpp self config --mirror GLOBAL

for attempt in 1 2 3; do
    if mcpp toolchain install llvm "$LLVM_VERSION"; then break; fi
    if [ "$attempt" = 3 ]; then exit 1; fi
    sleep 30
done
mcpp toolchain default "llvm@$LLVM_VERSION"
