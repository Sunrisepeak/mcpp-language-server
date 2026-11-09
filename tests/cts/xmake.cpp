// Projects xmake builds: its configuration, its modes and targets, a package it downloads and the
// state it keeps.
import mcppls.cts;

using namespace mcpplibs::mtest;
using namespace mcppls::cts;

namespace {

Fixture xmake_basic{"xmake-basic", on{"linux-x64"}};
Fixture xmake_late_config{"xmake-late-config", on{"linux-x64"}};
Fixture xmake_needs_download{"xmake-needs-download", on{"linux-x64"}};
Fixture xmake_no_standard{"xmake-no-standard", on{"linux-x64"}};
Fixture xmake_shared_tmp{"xmake-shared-tmp", on{"linux-x64"}};
Fixture xmake_stale_state{"xmake-stale-state", on{"linux-x64"}, resource{"machine"}};
Fixture xmake_target_then_mode{"xmake-target-then-mode", on{"linux-x64"}};
Fixture xmake_user_mode{"xmake-user-mode", on{"linux-x64"}};

}  // namespace
