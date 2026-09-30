// What tells a report that the process runs under PRoot, and the startup check that the
// preopened directories name "/" (the symptom of the register defect in an unpatched openkal).
import std;
import mcppls.os;
import mcppls.testing;
import mcppls.platform.env;
import mcppls.platform.preopen;
import mcppls.platform.sandbox;

namespace platform = mcppls::platform;

int main() {
    using namespace mcppls::testing;

    "the tracer's pid is read from the status text"_test = [] {
        expect(platform::tracer_pid("Name:\tmcppls\nTracerPid:\t4242\nUid:\t1000\n") == 4242);
        expect(platform::tracer_pid("Name:\tmcppls\nTracerPid:\t0\n") == 0);
        expect(platform::tracer_pid("Name:\tmcppls\n") == 0);
        expect(platform::tracer_pid("TracerPid:\tnot-a-number\n") == 0);
        expect(platform::tracer_pid("") == 0);
    };

    "a process traced by proot is in proot"_test = [] {
        platform::SandboxFacts facts { .selfStatus = "TracerPid:\t812\n", .tracerName = "proot\n" };
        expect(platform::classify_sandbox(facts) == "proot") << platform::classify_sandbox(facts);
        facts.tracerName = "proot-static";
        expect(platform::classify_sandbox(facts) == "proot");
    };

    "a debugger is not a sandbox"_test = [] {
        platform::SandboxFacts facts { .selfStatus = "TracerPid:\t812\n", .tracerName = "gdb\n" };
        expect(platform::classify_sandbox(facts).empty());
        facts.tracerName.clear();
        expect(platform::classify_sandbox(facts).empty());
    };

    "PROOT variables name proot when the tracer cannot be read"_test = [] {
        platform::SandboxFacts facts { .selfStatus = "TracerPid:\t812\n", .environment = { "PATH=/bin", "PROOT_TMP_DIR=/tmp" } };
        expect(platform::classify_sandbox(facts) == "proot");     // traced, tracer unnamed
        facts.selfStatus.clear();
        expect(platform::classify_sandbox(facts) == "proot");     // no status at all
        facts.environment = { "PATH=/bin", "NOT_PROOT_X=1" };
        expect(platform::classify_sandbox(facts).empty());
    };

    "a PROOT variable in a process nothing traces means nothing"_test = [] {
        const platform::SandboxFacts facts { .selfStatus = "Name:\tx\nTracerPid:\t0\n", .environment = { "PROOT_NO_SECCOMP=1" } };
        expect(platform::classify_sandbox(facts).empty());
    };

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
        if constexpr (mcppls::os::FAMILY != mcppls::os::Family::windows) {
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
