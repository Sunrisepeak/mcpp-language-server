// mcppls.pack.clangd ports trim_clangd.py's selection: bin/clangd[.exe], the one
// lib/clang/<major>/include it finds, and LICENSE.TXT, dropping everything else in the release.
//
// The win32-x64 platform is used for the selection/extraction tests because it is the one
// platform trim_clangd.py never strips or runs `--version` against (`--zip` bypasses the lock and
// network entirely), so the fixture's fake binary content never has to be a real executable; the
// win32-x64 fixture never matches this host's own mcppls::os::PLATFORM on the CI machines
// this runs on (Linux, macOS), so the "this host can run it" branch never fires for it either.
#include <archive.h>
#include <archive_entry.h>

import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.clangd;
import mcppls.pack.lock;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.os;

namespace clangd = mcppls::pack::clangd;
namespace lock = mcppls::pack::lock;
namespace base = mcppls::base;
namespace fs = mcppls::platform::fs;

namespace {

struct Member {
    std::string path;
    std::string content;
    bool executable { false };
};

bool write_zip(const std::string& path, const std::vector<Member>& members) {
    ::archive* out = ::archive_write_new();
    if (out == nullptr) return false;
    ::archive_write_set_format_zip(out);
    if (::archive_write_open_filename(out, path.c_str()) != ARCHIVE_OK) {
        ::archive_write_free(out);
        return false;
    }
    bool ok = true;
    for (const auto& member : members) {
        ::archive_entry* entry = ::archive_entry_new();
        ::archive_entry_set_pathname(entry, member.path.c_str());
        ::archive_entry_set_filetype(entry, AE_IFREG);
        ::archive_entry_set_size(entry, static_cast<std::int64_t>(member.content.size()));
        ::archive_entry_set_perm(entry, member.executable ? 0755 : 0644);
        if (::archive_write_header(out, entry) != ARCHIVE_OK) ok = false;
        if (ok && !member.content.empty()) {
            const auto wrote = ::archive_write_data(out, member.content.data(), member.content.size());
            ok = wrote == static_cast<::ssize_t>(member.content.size());
        }
        ::archive_entry_free(entry);
        if (!ok) break;
    }
    if (::archive_write_close(out) != ARCHIVE_OK) ok = false;
    ::archive_write_free(out);
    return ok;
}

// A clangd this host can actually start: this test program, which answers `--version` the way
// clangd does (see main). The code under test runs `clangd --version` when the platform being
// packaged is this host's, as the Python it replaced did. A shell script stood in for clangd before,
// and it runs on Linux and macOS but not on Windows, where every fixture that matched the host
// failed to start (openkal error 14). The same bytes serve all three hosts.
std::string runnable_clangd() {
    const auto self = mcppls::platform::env::arguments();
    std::string path { base::normalize_path(self.empty() ? std::string {} : self.front()) };
    if (!base::is_absolute_path(path)) path = base::join_path(fs::current_directory(), path);
    return fs::read_file(path).value_or(std::string {});
}

// A release shaped like the real clangd-win32-x64 zip, trimmed to what the selection cares about:
// one top directory, the executable, the license, one lib/clang/<major>/include, and the parts
// trim_clangd.py drops (a lib under the same major, a share/ tree).
//
// The "binary" is this test program (runnable_clangd): on the one host whose
// mcppls::os::PLATFORM matches the platform under test, the trim executes it to read
// `--version`, the way trim_clangd.py's `host_runs_it` did.
std::vector<Member> release_members(std::string_view exeName) {
    return {
        { std::format("clangd_23.1.0/bin/{}", exeName), runnable_clangd(), true },
        { "clangd_23.1.0/LICENSE.TXT", "Apache-2.0 WITH LLVM-exception", false },
        { "clangd_23.1.0/lib/clang/23/include/stddef.h", "typedef long ptrdiff_t;", false },
        { "clangd_23.1.0/lib/clang/23/lib/windows/somelib.lib", "not used by clangd", false },
        { "clangd_23.1.0/share/man/man1/clangd.1", "man page", false },
    };
}

lock::Lock empty_lock() { return lock::Lock {}; }

std::string scratch_dir() {
    const auto root = std::filesystem::current_path() / ".test-scratch"
                      / std::format("mcppls-clangd-{}", std::random_device {}());
    std::filesystem::create_directories(root);
    return root.string();
}

} // namespace

int main(int argc, char* argv[]) {
    // Started as the fixture's clangd (runnable_clangd): answer the one question asked of it.
    if (argc > 1 && std::string_view { argv[1] } == "--version") {
        std::println("clangd version 23.1.0 (fixture)");
        return 0;
    }
    using namespace mcppls::testing;

    const std::string work { scratch_dir() };
    const auto lockData = empty_lock();

    "win32-x64: the executable, the license and the one lib/clang/<major>/include survive"_test = [&] {
        const std::string zip { base::join_path(work, "clangd-win32.zip") };
        expect(fatal(write_zip(zip, release_members("clangd.exe"))));

        const std::string out { base::join_path(work, "win32-out") };
        auto result = clangd::trim(clangd::Options {
            .platform = "win32-x64",
            .outDirectory = out,
            .zip = zip,
            .cacheDirectory = base::join_path(work, "cache"),
            .strip = true,   // win32-x64 never strips; this checks that too
        }, lockData);
        expect(fatal(result.has_value())) << (result ? std::string {} : result.error().message);
        if (!result) return;

        expect(result->clangMajor == "23");
        expect(!result->stripped);
        expect(fs::is_regular_file(base::join_path(out, "bin/clangd.exe")));
        expect(fs::is_regular_file(base::join_path(out, "LICENSE.TXT")));
        expect(fs::is_regular_file(base::join_path(out, "lib/clang/23/include/stddef.h")));
        expect(!fs::exists(base::join_path(out, "lib/clang/23/lib/windows/somelib.lib")));
        expect(!fs::exists(base::join_path(out, "share")));
    };

    "linux-x64 and darwin-arm64 set the executable bit, win32-x64 does not"_test = [&] {
        for (const std::string_view platform : { "linux-x64", "darwin-arm64" }) {
            const std::string zip { base::join_path(work, std::format("clangd-{}.zip", platform)) };
            expect(fatal(write_zip(zip, release_members("clangd"))));
            const std::string out { base::join_path(work, std::format("{}-out", platform)) };
            auto result = clangd::trim(clangd::Options {
                .platform = std::string { platform },
                .outDirectory = out,
                .zip = zip,
                .cacheDirectory = base::join_path(work, "cache"),
                .strip = false,   // isolates the exec-bit check from strip/lipo, which need real tools
            }, lockData);
            expect(fatal(result.has_value())) << (result ? std::string {} : result.error().message);
            if (!result) continue;
            if constexpr (mcppls::os::FAMILY != mcppls::os::Family::windows) {
                const auto permissions = std::filesystem::status(result->binary).permissions();
                expect((permissions & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
            }
        }
    };

    "an archive with two top-level directories is refused"_test = [&] {
        const std::string zip { base::join_path(work, "two-roots.zip") };
        expect(fatal(write_zip(zip, { { "one/bin/clangd", "a", true }, { "two/bin/clangd", "b", true } })));
        auto result = clangd::trim(clangd::Options {
            .platform = "win32-x64", .outDirectory = base::join_path(work, "two-roots-out"),
            .zip = zip, .cacheDirectory = base::join_path(work, "cache"), .strip = false,
        }, lockData);
        expect(!result.has_value());
        if (result) return;
        expect(result.error().code == "clangd-shape");
    };

    "an archive with no lib/clang/<major> is refused"_test = [&] {
        const std::string zip { base::join_path(work, "no-major.zip") };
        expect(fatal(write_zip(zip, { { "top/bin/clangd.exe", "MZ", true }, { "top/LICENSE.TXT", "x", false } })));
        auto result = clangd::trim(clangd::Options {
            .platform = "win32-x64", .outDirectory = base::join_path(work, "no-major-out"),
            .zip = zip, .cacheDirectory = base::join_path(work, "cache"), .strip = false,
        }, lockData);
        expect(!result.has_value());
        if (result) return;
        expect(result.error().code == "clangd-shape");
    };

    "an unknown platform is refused before anything is read"_test = [&] {
        auto result = clangd::trim(clangd::Options {
            .platform = "unknown-platform", .outDirectory = base::join_path(work, "unknown-out"),
            .zip = base::join_path(work, "does-not-need-to-exist.zip"), .cacheDirectory = "", .strip = false,
        }, lockData);
        expect(!result.has_value());
        if (result) return;
        expect(result.error().code == "clangd-platform");
    };

    "a darwin-arm64 thin-and-strip needs a macOS host"_test = [&] {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::macos) {
            // This is exactly the host the real thing needs; nothing to assert about refusing it.
        } else {
            const std::string zip { base::join_path(work, "clangd-darwin-strip.zip") };
            expect(fatal(write_zip(zip, release_members("clangd"))));
            auto result = clangd::trim(clangd::Options {
                .platform = "darwin-arm64", .outDirectory = base::join_path(work, "darwin-strip-out"),
                .zip = zip, .cacheDirectory = base::join_path(work, "cache"), .strip = true,
            }, lockData);
            expect(!result.has_value());
            if (result) return;
            expect(result.error().code == "clangd-host");
        }
    };

    // trim_clangd.py's `host_runs_it`: only fires when the platform under trim matches this host,
    // and reads the first line of `--version` when it does. Both sides of that are exercised in
    // one test by trimming for this host's own PLATFORM (which matches, and reads the
    // fixture script's line) and for a platform two steps away in the tuple (which never does).
    "the version line is read only when this host can run what was just trimmed"_test = [&] {
        const std::string here { mcppls::os::PLATFORM };
        const std::string elsewhere { here == "linux-x64" ? "darwin-arm64" : "linux-x64" };

        const std::string hereZip { base::join_path(work, "clangd-here.zip") };
        expect(fatal(write_zip(hereZip, release_members(here == "win32-x64" ? "clangd.exe" : "clangd"))));
        auto matched = clangd::trim(clangd::Options {
            .platform = here, .outDirectory = base::join_path(work, "here-out"),
            .zip = hereZip, .cacheDirectory = base::join_path(work, "cache"), .strip = false,
        }, lockData);
        expect(fatal(matched.has_value())) << (matched ? std::string {} : matched.error().message);
        if (matched) {
            expect(matched->versionLine.has_value());
            expect(matched->versionLine.value_or("").contains("23.1.0"));
        }

        const std::string elsewhereZip { base::join_path(work, "clangd-elsewhere.zip") };
        expect(fatal(write_zip(elsewhereZip, release_members(elsewhere == "win32-x64" ? "clangd.exe" : "clangd"))));
        auto unmatched = clangd::trim(clangd::Options {
            .platform = elsewhere, .outDirectory = base::join_path(work, "elsewhere-out"),
            .zip = elsewhereZip, .cacheDirectory = base::join_path(work, "cache"), .strip = false,
        }, lockData);
        expect(fatal(unmatched.has_value())) << (unmatched ? std::string {} : unmatched.error().message);
        if (unmatched) expect(!unmatched->versionLine.has_value());
    };

    std::filesystem::remove_all(work);
    return report();
}
