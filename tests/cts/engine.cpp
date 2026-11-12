// The engine: clangd that cannot start, crashes or exits, a module that fails to build and what
// imports it, a payload that is corrupt, the cache's budget and reset, partial scans, and the
// workarounds of clangd's defects.
import mcppls.cts;

using namespace mcpplibs::mtest;
using namespace mcppls::cts;

namespace {

Fixture cache_budget{"cache-budget", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture clangd_cannot_load{"clangd-cannot-load", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture clangd_crash_closure{"clangd-crash-closure", on{"linux-x64", "linux-arm64", "darwin-arm64"}, resource{"machine"}};
Fixture clangd_crash_context{"clangd-crash-context", on{"linux-x64", "linux-arm64", "darwin-arm64"}, resource{"machine"}};
Fixture clangd_exit_strike{"clangd-exit-strike", on{"linux-x64", "linux-arm64", "darwin-arm64"}, resource{"machine"}};
Fixture diagnostic_bundle{"diagnostic-bundle", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture engine_restarts{"engine-restarts", on{"linux-x64", "linux-arm64", "win32-x64"}, resource{"machine"}};
Fixture engine_trap{"engine-trap", on{"linux-x64", "linux-arm64", "darwin-arm64"}, resource{"machine"}};
Fixture engine_none{"engine-none", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture failure_at_base{"failure-at-base", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture failure_at_base_neovim{"failure-at-base@neovim", on{"linux-x64"}, resource{"machine"}};
Fixture failure_at_base_vscode{"failure-at-base@vscode", on{"linux-x64", "win32-x64"}, resource{"machine"}};
Fixture failure_at_base_zed{"failure-at-base@zed", on{"darwin-arm64"}, resource{"machine"}};
Fixture module_fails_in_import{"module-fails-in-import", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture module_faults{"module-faults", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture module_faults_zed{"module-faults@zed", on{"linux-x64"}, resource{"machine"}};
Fixture partial_scan_standins{"partial-scan-standins", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture payload_corrupt{"payload-corrupt", on{"linux-x64", "linux-arm64"}};
Fixture preparation_after_idle{"preparation-after-idle", on{"linux-x64", "win32-x64"}};
Fixture provisional_no_prime{"provisional-no-prime", on{"linux-x64", "darwin-arm64"}};
Fixture reset_cache{"reset-cache", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture reset_cache_linked{"reset-cache@linked", on{"linux-x64"}};
Fixture workaround_canaries{"workaround-canaries", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};

}  // namespace
