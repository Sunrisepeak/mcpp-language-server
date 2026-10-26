// What tells a report that the process runs under PRoot (mcpplibs.os.linux classifies it; its own
// tests hold the cases), and the startup check that the preopened directories name "/".
import std;
import mcppls.os;
import mcppls.testing;
import mcppls.platform.env;
import mcppls.platform.preopen;
import mcppls.platform.sandbox;

namespace platform = mcppls::platform;

int main() {
    using namespace mcppls::testing;

    "this process answers the same way twice"_test = [] {
        expect(platform::sandbox() == platform::sandbox());
        expect(platform::sandbox().empty() || platform::sandbox() == "proot") << platform::sandbox();
    };

    // Set by the `proot` CI job, which runs this test under a real PRoot: detection is checked
    // against the real thing, in both of PRoot's modes.
    "this process is where the environment says it is"_test = [] {
        const auto expected = mcppls::platform::env::get("MCPPLS_EXPECT_SANDBOX");
        if (!expected) return;
        expect(platform::sandbox() == *expected) << "detected '" << platform::sandbox() << "', expected '" << *expected << "'";
    };

    "a table with a directory named / is fine"_test = [] {
        if (mcppls::os::family() != mcppls::os::Family::windows) {
            expect(platform::has_root_preopen(std::array<std::string, 2> { "/", "/home/x" }));
            expect(!platform::has_root_preopen(std::array<std::string, 1> { "/home/x" }));
            expect(!platform::has_root_preopen(std::array<std::string, 2> { std::string { "\x10\x9b\xff\x7f", 4 }, "/home/x" }));
            expect(!platform::has_root_preopen(std::span<const std::string> {}));
        }
    };

    "this process was given a directory named /"_test = [] {
        const auto names = platform::preopen_names();
        expect(platform::has_root_preopen(names)) << names.size();
    };
    return report();
}
