// What replaced Python's `tarfile` and `zipfile` has to behave the way packaging relied on them
// behaving, so the fixtures here are written with libarchive's own writer and read back: a top
// directory that gets stripped, a selection, an executable bit, a symlink materialized as a copy,
// and the two paths an archive could use to write outside the directory it was given.
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.archive;
import mcppls.platform.fs;
import mcppls.os;
import openkal.fs;

namespace ar = mcppls::pack::archive;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

struct Member {
    std::string path;
    std::string content;
    bool executable { false };
    std::string symlinkTo;   // non-empty makes it a symlink
};

// Writes `members` as one archive. `format` is "paxr" for tar.gz or "zip".
// A FIXTURE THAT FAILS SHOULD SAY WHY, BECAUSE THIS ONE FAILS ON ONE PLATFORM
// ONLY. On the Windows runner every assertion below the two `write_archive`
// calls has been red while the same source passes natively on Linux and under
// Wine, and a bare `false` names neither the call that failed nor libarchive's
// reason for it. The path is printed with it: `temp_directory_path()` is a
// native Windows path here and the C library underneath is musl presenting
// POSIX, which is the difference most likely to matter.
bool write_archive(const std::string& path, std::string_view format, const std::vector<Member>& members) {
    auto refuse = [&](const char* where, ::archive* a) {
        std::println(stderr, "write_archive: {} failed for '{}': {} (errno {})",
                     where, path,
                     a != nullptr && ::archive_error_string(a) != nullptr
                         ? ::archive_error_string(a) : "no message",
                     a != nullptr ? ::archive_errno(a) : 0);
        // EACCES HERE IS A CAPABILITY ANSWER, NOT A PERMISSION ONE. openkal
        // names a file relative to a directory the program was given, so a
        // path beneath none of them is refused rather than searched. The
        // Windows runner reports errno 13 for a temp directory whose name
        // arrives in 8.3 short form (`C:/Users/RUNNER~1/...`), and the
        // question that settles it is which spelling the program holds --
        // so it prints them rather than leaving it to be inferred.
        const kal_uintptr count { kal_fs_preopen_count() };
        std::println(stderr, "  the program holds {} preopened director{}:",
                     count, count == 1 ? "y" : "ies");
        for (kal_uintptr i = 0; i < count; ++i) {
            kal_dir dir {};
            std::string name(1024, '\0');
            kal_uintptr length {};
            if (kal_fs_preopen(i, &dir, name.data(), name.size(), &length) != kal_ok) {
                std::println(stderr, "    [{}] <could not be read>", i);
                continue;
            }
            name.resize(length);
            std::println(stderr, "    [{}] '{}'", i, name);
        }
        return false;
    };
    ::archive* out = ::archive_write_new();
    if (out == nullptr) return refuse("archive_write_new", nullptr);
    if (format == "zip") {
        ::archive_write_set_format_zip(out);
    } else {
        ::archive_write_set_format_pax_restricted(out);
        ::archive_write_add_filter_gzip(out);
    }
    if (::archive_write_open_filename(out, path.c_str()) != ARCHIVE_OK) {
        const bool r = refuse("archive_write_open_filename", out);
        ::archive_write_free(out);
        return r;
    }
    bool ok = true;
    for (const auto& member : members) {
        ::archive_entry* entry = ::archive_entry_new();
        ::archive_entry_set_pathname(entry, member.path.c_str());
        if (!member.symlinkTo.empty()) {
            ::archive_entry_set_filetype(entry, AE_IFLNK);
            ::archive_entry_set_symlink(entry, member.symlinkTo.c_str());
            ::archive_entry_set_size(entry, 0);
            ::archive_entry_set_perm(entry, 0777);
        } else {
            ::archive_entry_set_filetype(entry, AE_IFREG);
            ::archive_entry_set_size(entry, static_cast<std::int64_t>(member.content.size()));
            ::archive_entry_set_perm(entry, member.executable ? 0755 : 0644);
        }
        if (::archive_write_header(out, entry) != ARCHIVE_OK) ok = refuse("archive_write_header", out);
        if (ok && !member.content.empty()) {
            const auto wrote = ::archive_write_data(out, member.content.data(), member.content.size());
            if (wrote != static_cast<::ssize_t>(member.content.size())) {
                std::println(stderr, "write_archive: archive_write_data wrote {} of {} for '{}': {}",
                             wrote, member.content.size(), member.path,
                             ::archive_error_string(out) != nullptr ? ::archive_error_string(out) : "no message");
                ok = false;
            }
        }
        ::archive_entry_free(entry);
        if (!ok) break;
    }
    if (::archive_write_close(out) != ARCHIVE_OK && ok) ok = refuse("archive_write_close", out);
    ::archive_write_free(out);
    return ok;
}

// A SCRATCH DIRECTORY BESIDE THE PROGRAM, NOT THE SYSTEM TEMP, AND THE REASON
// IS MEASURED RATHER THAN PREFERRED.
//
// On the Windows runner every write beneath the system temp failed with
//
//     Couldn't stat 'C:/Users/RUNNER~1/AppData/Local/Temp/.../list.tar.gz'
//       (errno 13)
//
// and the program's own preopen list rules out the reading EACCES usually has
// there, because it holds `C:` outright:
//
//     [0] 'D:/a/mcpp-language-server/mcpp-language-server'
//     [1] 'C:'
//     [2] 'D:'
//
// The name is inside a directory the program was given and resolving it still
// fails. `RUNNER~1` is an 8.3 short name --- the account name does not fit in
// eight characters --- and BOTH `std::filesystem::temp_directory_path()` and
// `TEMP`/`TMP` carry that form on this runner, so choosing between them
// changes nothing. It was measured: switching helpers left the path, and the
// failure, identical.
//
// What does differ is which layer touches the name.
// `std::filesystem::create_directories` reaches Win32 and creates the
// directory successfully; libarchive's `stat` reaches it through musl and
// openkal and does not. The two disagree about the same string.
//
// So the fixture writes beneath the directory the program is preopened on
// directly, which has no 8.3 component and is reachable by both layers. That
// is also simply better for a test: its scratch space is beside its own
// build rather than in a shared location.
//
// It does not answer the question underneath, and is not meant to: whether
// openkal should resolve an 8.3 component of a path beneath a directory the
// program holds is a question for openkal, and it now has a reproduction, the
// preopen list that goes with it, and a second layer that accepts the same
// name.
std::string temp_dir(std::string_view name) {
    const auto root = std::filesystem::current_path()
                      / ".test-scratch"
                      / std::format("mcppls-archive-{}-{}", name, std::random_device {}());
    std::filesystem::create_directories(root);
    return root.string();
}

bool contains(const std::vector<std::string>& all, std::string_view one) {
    return std::ranges::find(all, one) != all.end();
}

} // namespace

int main() {
    using namespace mcppls::testing;

    const std::string work { temp_dir("work") };

    // WHAT `fstat` SAYS ABOUT A DESCRIPTOR THE PROGRAM JUST OPENED.
    //
    // `archive_write_open_filename.c:178` OPENS AND `:189` FSTATS, and it is
    // the second that fails:
    //
    //     mine->fd = open(mbs, flags, 0666);   // succeeds, the file exists
    //     if (fstat(mine->fd, &st) != 0)       // EACCES
    //         archive_set_error(a, errno, "Couldn't stat '%s'", mbs);
    //
    // Which is why every path-shaped explanation died: the path had already
    // succeeded by then. Three guesses were spent on the name --- that it lay
    // outside the program's preopened directories, that the two temp-directory
    // helpers disagreed about its 8.3 form, that the 8.3 component was what
    // failed --- and a path with no short component directly beneath a held
    // directory fails identically. `stat` on that same path answers correctly.
    //
    // So the question is about the DESCRIPTOR, not the name, and this asks it
    // in four calls rather than through an archive writer.
    "fstat answers for a descriptor this program just opened"_test = [&] {
        const std::string missing { base::join_path(work, "definitely-not-here") };
        struct ::stat st {};
        errno = 0;
        const int rc { ::stat(missing.c_str(), &st) };
        const int missingErrno { errno };
        std::println(stderr, "stat('{}') -> rc={} errno={} ({})",
                     missing, rc, missingErrno, std::strerror(missingErrno));
        expect(rc != 0);
        expect(missingErrno == ENOENT);

        const std::string made { base::join_path(work, "opened-here") };
        errno = 0;
        const int fd { ::open(made.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) };
        std::println(stderr, "open('{}') -> fd={} errno={} ({})",
                     made, fd, errno, std::strerror(errno));
        expect(fd >= 0);
        if (fd < 0) return;

        struct ::stat fdSt {};
        errno = 0;
        const int fstatRc { ::fstat(fd, &fdSt) };
        const int fstatErrno { errno };
        std::println(stderr, "fstat({}) -> rc={} errno={} ({}) size={}",
                     fd, fstatRc, fstatErrno, std::strerror(fstatErrno),
                     static_cast<long long>(fdSt.st_size));
        ::close(fd);

        // THE ONE THAT MATTERS. A descriptor this program opened a line ago is
        // valid by construction, so `fstat` on it has nothing to refuse.
        expect(fstatRc == 0);
        expect(fstatErrno == 0);
    };

    "a tar.gz is listed without writing anything"_test = [&] {
        const std::string path { base::join_path(work, "list.tar.gz") };
        expect(write_archive(path, "paxr",
                             { { "top/README", "hello", false, "" },
                               { "top/bin/tool", "binary", true, "" } }));
        auto entries = ar::list(path);
        expect(entries.has_value());
        expect(entries->size() == 2);
        const bool sawExecutable = std::ranges::any_of(
            *entries, [](const ar::Entry& e) { return e.path == "top/bin/tool" && e.executable; });
        expect(sawExecutable);
    };

    "the top-level directory is stripped and the selection honoured"_test = [&] {
        const std::string path { base::join_path(work, "select.tar.gz") };
        const std::string out { base::join_path(work, "select-out") };
        expect(write_archive(path, "paxr",
                             { { "clangd_20/bin/clangd", "ELF", true, "" },
                               { "clangd_20/lib/clang/20/include/stddef.h", "typedef", false, "" },
                               { "clangd_20/share/man/clangd.1", "man page", false, "" } }));
        auto written = ar::extract(path, out, [](std::string_view relative) {
            return relative == "bin/clangd" || relative.starts_with("lib/clang/");
        });
        // `expect` RECORDS AND CONTINUES, so a failed `has_value()` is not a
        // stop: every dereference below it runs anyway. That is how a fixture
        // this test could not write turned into an access violation, twice,
        // under two different root causes. The guard costs one branch and
        // makes the reported failure the one that happened.
        expect(written.has_value());
        if (!written) return;
        expect(written->size() == 2);
        expect(contains(*written, "bin/clangd"));
        expect(!contains(*written, "share/man/clangd.1"));
        expect(fs::is_regular_file(base::join_path(out, "bin/clangd")));
        expect(!fs::exists(base::join_path(out, "share/man/clangd.1")));

        // THE EXECUTE BIT IS A POSIX PROPERTY, AND WINDOWS HAS NO OPINION.
        //
        // What makes a file runnable there is its extension, so the extractor
        // cannot have set a bit and cannot have failed to set one. Asserting
        // the POSIX mode anyway made this test fail on a platform that was
        // behaving correctly. Skipping it there would make "no such concept"
        // and "we forgot to set it" the same reading, so the branch asserts
        // what distinguishes them: the executable entry and a data entry from
        // the same archive must be INDISTINGUISHABLE where no bit exists, and
        // must differ where one does.
        const auto exeMode = std::filesystem::status(base::join_path(out, "bin/clangd")).permissions();
        const auto dataMode =
            std::filesystem::status(base::join_path(out, "lib/clang/20/include/stddef.h")).permissions();
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            expect(exeMode == dataMode);
        } else {
            expect((exeMode & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
            expect((dataMode & std::filesystem::perms::owner_exec) == std::filesystem::perms::none);
        }
    };

    "a symlink inside the selection becomes a copy"_test = [&] {
        const std::string path { base::join_path(work, "links.tar.gz") };
        const std::string out { base::join_path(work, "links-out") };
        expect(write_archive(path, "paxr",
                             { { "kit/lib/libc++.so.1", "REAL", false, "" },
                               { "kit/lib/libc++.so", "", false, "libc++.so.1" } }));
        auto written = ar::extract(path, out, [](std::string_view) { return true; });
        expect(written.has_value());
        const std::string copied { base::join_path(out, "lib/libc++.so") };
        expect(fs::is_regular_file(copied));
        expect(!std::filesystem::is_symlink(copied));
        auto content = fs::read_file(copied);
        expect(content.has_value());
        expect(*content == "REAL");
    };

    "a link pointing outside the selection is skipped, not followed"_test = [&] {
        const std::string path { base::join_path(work, "dangling.tar.gz") };
        const std::string out { base::join_path(work, "dangling-out") };
        expect(write_archive(path, "paxr",
                             { { "kit/keep/file", "kept", false, "" },
                               { "kit/keep/alias", "", false, "../skip/file" },
                               { "kit/skip/file", "skipped", false, "" } }));
        auto written = ar::extract(path, out, [](std::string_view relative) {
            return relative.starts_with("keep/");
        });
        expect(written.has_value());
        expect(contains(*written, "keep/file"));
        expect(!fs::exists(base::join_path(out, "keep/alias")));
    };

    // The one above is skipped because its target is outside the selection. This one climbs with
    // `..` too and stays inside, so it must be copied: `..` is resolved, not refused on sight.
    "a link that climbs with .. but stays inside the selection becomes a copy"_test = [&] {
        const std::string path { base::join_path(work, "climb.tar.gz") };
        const std::string out { base::join_path(work, "climb-out") };
        expect(write_archive(path, "paxr",
                             { { "kit/lib/real", "REAL", true, "" },
                               { "kit/bin/alias", "", false, "../lib/real" } }));
        auto written = ar::extract(path, out, [](std::string_view) { return true; });
        expect(written.has_value());
        if (!written) return;
        expect(contains(*written, "bin/alias"));
        auto content = fs::read_file(base::join_path(out, "bin/alias"));
        expect(content.has_value() && *content == "REAL");
    };

    "a link to a link is followed to the file"_test = [&] {
        const std::string path { base::join_path(work, "chain.tar.gz") };
        const std::string out { base::join_path(work, "chain-out") };
        // Listed link-first on purpose: neither link's target exists when it is read.
        expect(write_archive(path, "paxr",
                             { { "kit/lib/libc++.so", "", false, "libc++.so.1" },
                               { "kit/lib/libc++.so.1", "", false, "libc++.so.1.0" },
                               { "kit/lib/libc++.so.1.0", "REAL", false, "" } }));
        auto written = ar::extract(path, out, [](std::string_view) { return true; });
        expect(written.has_value());
        if (!written) return;
        expect(written->size() == 3);
        auto content = fs::read_file(base::join_path(out, "lib/libc++.so"));
        expect(content.has_value() && *content == "REAL");
    };

    "a file beside the top-level directory is refused, not dropped"_test = [&] {
        const std::string path { base::join_path(work, "stray.tar.gz") };
        expect(write_archive(path, "paxr",
                             { { "top/file", "a", false, "" }, { "README", "b", false, "" } }));
        auto written = ar::extract(path, base::join_path(work, "stray-out"), [](std::string_view) { return true; });
        expect(!written.has_value());
        if (written) return;
        expect(written.error().code == "archive-shape");
    };

    // Refused on every host alike, so an archive cannot extract on Linux and escape on Windows.
    "a backslash climb or a drive name writes nothing"_test = [&] {
        const std::string path { base::join_path(work, "windows-escape.tar.gz") };
        const std::string out { base::join_path(work, "windows-escape-out") };
        expect(write_archive(path, "paxr",
                             { { "top/ok", "fine", false, "" },
                               { "top/a\\..\\..\\..\\escaped", "no", false, "" },
                               { "top/C:evil", "no", false, "" } }));
        auto written = ar::extract(path, out, [](std::string_view) { return true; });
        expect(written.has_value());
        if (!written) return;
        expect(written->size() == 1);
        expect(contains(*written, "ok"));
    };

    "zip is read by the same call"_test = [&] {
        const std::string path { base::join_path(work, "release.zip") };
        const std::string out { base::join_path(work, "zip-out") };
        expect(write_archive(path, "zip",
                             { { "clangd_20/bin/clangd.exe", "MZ", false, "" },
                               { "clangd_20/LICENSE.TXT", "license", false, "" } }));
        auto written = ar::extract(path, out, [](std::string_view relative) {
            return relative == "bin/clangd.exe";
        });
        expect(written.has_value());
        expect(written->size() == 1);
        expect(fs::is_regular_file(base::join_path(out, "bin/clangd.exe")));
        expect(!fs::exists(base::join_path(out, "LICENSE.TXT")));
    };

    "an archive with two top-level directories is refused, not guessed at"_test = [&] {
        const std::string path { base::join_path(work, "two-roots.tar.gz") };
        expect(write_archive(path, "paxr",
                             { { "one/file", "a", false, "" }, { "two/file", "b", false, "" } }));
        auto written = ar::extract(path, base::join_path(work, "two-roots-out"),
                                        [](std::string_view) { return true; });
        expect(!written.has_value());
        expect(written.error().code == "archive-shape");
    };

    "an entry climbing out of the directory writes nothing"_test = [&] {
        const std::string path { base::join_path(work, "escape.tar.gz") };
        const std::string out { base::join_path(work, "escape-out") };
        expect(write_archive(path, "paxr",
                             { { "top/ok", "fine", false, "" },
                               { "top/../../escaped", "should not be written", false, "" } }));
        auto written = ar::extract(path, out, [](std::string_view) { return true; });
        expect(written.has_value());
        expect(contains(*written, "ok"));
        expect(!fs::exists(base::join_path(base::parent_path(base::parent_path(out)), "escaped")));
    };

    "a missing archive is an error rather than an empty result"_test = [&] {
        auto entries = ar::list(base::join_path(work, "there-is-no-such-file.tar.gz"));
        expect(!entries.has_value());
    };

    // ---- the writer (xlings_artifacts.py's write_archive, ported) -----------------------------

    "a written archive round-trips through list and extract"_test = [&] {
        const std::string sourceDir { base::join_path(work, "write-source") };
        expect(fs::create_directories(base::join_path(sourceDir, "bin")).has_value());
        expect(fs::create_directories(base::join_path(sourceDir, "kit/include")).has_value());
        expect(fs::write_file(base::join_path(sourceDir, "bin/tool"), "ELF-ish").has_value());
        expect(fs::make_executable(std::array { base::join_path(sourceDir, "bin/tool") }).has_value());
        expect(fs::write_file(base::join_path(sourceDir, "kit/include/header.h"), "typedef").has_value());
        expect(fs::write_file(base::join_path(sourceDir, "LICENSE"), "Apache-2.0").has_value());

        const std::string archivePath { base::join_path(work, "written.tar.gz") };
        const std::vector<ar::WriteEntry> entries {
            { base::join_path(sourceDir, "bin/tool"), "bin/tool" },
            { base::join_path(sourceDir, "kit"), "kit" },
            { base::join_path(sourceDir, "LICENSE"), "LICENSE" },
        };
        auto wrote = ar::write(archivePath, "mcppls-1.0-linux-x86_64", entries);
        expect(wrote.has_value());
        if (!wrote) return;

        auto listed = ar::list(archivePath);
        expect(listed.has_value());
        if (!listed) return;
        auto executable_bit = [&](std::string_view path) {
            const auto found = std::ranges::find_if(*listed, [&](const ar::Entry& e) { return e.path == path; });
            return found != listed->end() && found->executable;
        };
        // The writer carries the source file's execute bit, and on Windows a file has none to carry
        // (the extraction test above says why that is not a no-op to assert away). So there the
        // program and the data file must look alike; elsewhere they must differ.
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            expect(executable_bit("mcppls-1.0-linux-x86_64/bin/tool") == executable_bit("mcppls-1.0-linux-x86_64/LICENSE"));
        } else {
            expect(executable_bit("mcppls-1.0-linux-x86_64/bin/tool"));
            expect(!executable_bit("mcppls-1.0-linux-x86_64/LICENSE"));
        }
        const bool sawHeader = std::ranges::any_of(
            *listed, [](const ar::Entry& e) { return e.path == "mcppls-1.0-linux-x86_64/kit/include/header.h"; });
        expect(sawHeader);

        const std::string out { base::join_path(work, "written-out") };
        auto extracted = ar::extract(archivePath, out, [](std::string_view) { return true; });
        expect(extracted.has_value());
        if (!extracted) return;
        expect(contains(*extracted, "bin/tool"));
        expect(contains(*extracted, "kit/include/header.h"));
        expect(contains(*extracted, "LICENSE"));
        auto header = fs::read_file(base::join_path(out, "kit/include/header.h"));
        expect(header.has_value() && *header == "typedef");
        const auto toolMode = std::filesystem::status(base::join_path(out, "bin/tool")).permissions();
        if constexpr (mcppls::os::FAMILY != mcppls::os::Family::windows) {
            expect((toolMode & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
        }
    };

    "writing to a directory that cannot be created is an error, not a crash"_test = [&] {
        auto wrote = ar::write(base::join_path(work, "definitely/missing/parent/out.tar.gz"), "root",
                               std::vector<ar::WriteEntry> { { base::join_path(work, "there-is-no-such-file.tar.gz"), "x" } });
        expect(!wrote.has_value());
    };

    std::filesystem::remove_all(work);
    // WAS `return 0;`, WHICH MADE EVERY `expect()` ABOVE ADVISORY ONLY. `mcpp test` passes a test
    // by its exit code, and nothing here fed a failed expectation into one; the new write() tests
    // added alongside this fix are the first ones in this file that would actually have caught
    // anything, so the gap is closed rather than carried forward.
    return report();
}
