// The target of an mcpp project: which one the server selects, a selection the user states, one
// that does not match or does not exist, and the fallback when the description lags.
import mcppls.cts;

using namespace mcpplibs::mtest;
using namespace mcppls::cts;

namespace {

Fixture mcpp_target_auto_fallback{"mcpp-target-auto-fallback", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_build_tool_off{"mcpp-target-build-tool-off", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_discovery_off{"mcpp-target-discovery-off", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_explicit{"mcpp-target-explicit", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_fallback_mismatch{"mcpp-target-fallback-mismatch", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_fallback_preserve{"mcpp-target-fallback-preserve", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_invalid{"mcpp-target-invalid", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_live{"mcpp-target-live", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_mismatch{"mcpp-target-mismatch", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_mismatch_cold{"mcpp-target-mismatch-cold", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_project{"mcpp-target-project", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_provider_excluded{"mcpp-target-provider-excluded", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_race{"mcpp-target-race", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_selection{"mcpp-target-selection", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_selection_polling{"mcpp-target-selection@polling", on{"linux-x64"}};
Fixture mcpp_target_windows{"mcpp-target-windows", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_windows_real{"mcpp-target-windows-real", on{"win32-x64"}, [] { run({.fixture = "mcpp-target-windows-real", .evidence = true}); }};

}  // namespace
