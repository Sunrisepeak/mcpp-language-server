// Projects with no build description: what the server infers from the sources, more than one
// root, no modules at all, a workspace that is not trusted, and no semantic kit installed.
import mcppls.cts;

using namespace mcpplibs::mtest;
using namespace mcppls::cts;

namespace {

Fixture inferred{"inferred", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture inferred_bom{"inferred-bom", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture inferred_cxx26{"inferred-cxx26", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture inferred_msvc{"inferred-msvc", on{"win32-x64"}};
Fixture kit_openkal{"kit-openkal", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture multi_root{"multi-root", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture no_modules{"no-modules", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture untrusted{"untrusted", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};

}  // namespace
