// mcppls.pack.targets reads a platform name as `<os>-<arch>`; which platforms exist is the lock's to
// say. These are the facts every packaging step derives from a name instead of comparing it to a
// literal: the suffix, xlings' spelling, Apple's architecture name.
import std;
import mcppls.testing;
import mcppls.pack.targets;

namespace targets = mcppls::pack::targets;

int main() {
    using namespace mcppls::testing;

    "every platform the lock has today reads as its os and arch"_test = [] {
        const auto linuxX64 = targets::parse("linux-x64");
        const auto linuxArm = targets::parse("linux-arm64");
        const auto darwin = targets::parse("darwin-arm64");
        const auto windows = targets::parse("win32-x64");
        expect(fatal(linuxX64 && linuxArm && darwin && windows));
        if (!linuxX64 || !linuxArm || !darwin || !windows) return;
        expect(linuxX64->os == targets::Os::linux && linuxX64->arch == targets::Arch::x64);
        expect(linuxArm->os == targets::Os::linux && linuxArm->arch == targets::Arch::arm64);
        expect(darwin->os == targets::Os::darwin && darwin->arch == targets::Arch::arm64);
        expect(windows->os == targets::Os::win32 && windows->arch == targets::Arch::x64);
    };

    "anything that is not <os>-<arch> is not a platform"_test = [] {
        for (const std::string_view name : { "", "linux", "linux-", "-x64", "linux-x86_64", "macos-arm64", "unknown-platform", "linux-x64-gnu" }) {
            expect(!targets::parse(name).has_value()) << name;
        }
    };

    "only win32 has an executable suffix"_test = [] {
        expect(targets::executable_suffix(*targets::parse("win32-x64")) == ".exe");
        expect(targets::executable_suffix(*targets::parse("linux-arm64")).empty());
        expect(targets::executable_suffix(*targets::parse("darwin-arm64")).empty());
    };

    "xlings and Apple spell the pair their own way"_test = [] {
        expect(targets::xlings_os(targets::Os::darwin) == "macosx");
        expect(targets::xlings_os(targets::Os::linux) == "linux");
        expect(targets::xlings_os(targets::Os::win32) == "windows");
        expect(targets::xlings_arch(targets::Arch::arm64) == "aarch64");
        expect(targets::xlings_arch(targets::Arch::x64) == "x86_64");
        expect(targets::apple_arch(targets::Arch::arm64) == "arm64");
        expect(targets::apple_arch(targets::Arch::x64) == "x86_64");
    };

    return report();
}
