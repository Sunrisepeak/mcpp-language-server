// What tells a report that the process runs under PRoot (mcpplibs.os.linux classifies it; its own
// tests hold the cases), and that the directories this program was given name "/".
import std;
import mcpplibs.os;
import mcppls.testing;
import mcppls.toolrun;

namespace os = mcpplibs::os;
namespace toolrun = mcppls::toolrun;

int main() {
    using namespace mcppls::testing;

    "this process answers the same way twice"_test = [] {
        expect(toolrun::sandbox() == toolrun::sandbox());
        expect(toolrun::sandbox().empty() || toolrun::sandbox() == "proot") << toolrun::sandbox();
    };

    // Set by the `proot` CI job, which runs this test under a real PRoot: detection is checked
    // against the real thing, in both of PRoot's modes.
    "this process is where the environment says it is"_test = [] {
        const auto expected = os::env::get("MCPPLS_EXPECT_SANDBOX");
        if (!expected) return;
        expect(toolrun::sandbox() == *expected) << "detected '" << toolrun::sandbox() << "', expected '" << *expected << "'";
    };

    // A program is started by its absolute name, which must lie beneath a directory this one was
    // given (mcpplibs.os `fs::locate`): on POSIX that is the directory named "/".
    "this process was given a directory named /"_test = [] {
        if (os::host() == os::platform::windows) return;
        const auto names = os::fs::preopens();
        expect(std::ranges::find(names, std::string { "/" }) != names.end()) << names.size();
    };
    return report();
}
