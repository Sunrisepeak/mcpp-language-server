module mcppls.pack.clangd;

import std;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.os;
import mcppls.pack.archive;
import mcppls.pack.fetch;
import mcppls.pack.lock;
import mcppls.pack.targets;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;

namespace mcppls::pack::clangd {
namespace fs = mcppls::platform::fs;
namespace env = mcppls::platform::env;
namespace platform = mcppls::platform;
namespace {


// The archive's sole top-level directory and its sole lib/clang/<major>, found the way
// trim_clangd.py's own `extract()` found them: from the raw archive listing, before anything is
// written, so the selection passed to mcppls.pack.archive::extract already knows the paths
// (relative to that top-level directory) it needs to keep.
struct Shape {
    std::string major;
};

base::Result<Shape> shape_of(std::string_view archivePath, const std::vector<archive::Entry>& entries) {
    std::set<std::string> roots;
    for (const auto& item : entries) {
        if (const auto slash = item.path.find('/'); slash != std::string::npos) roots.insert(item.path.substr(0, slash));
    }
    if (roots.size() != 1) {
        std::vector<std::string> found { roots.begin(), roots.end() };
        return base::fail("clangd-shape", std::format("expected one top-level directory in {}, found {}",
                                                       archivePath, found.empty() ? "none" : base::join(found, ", ")));
    }
    const std::string root { *roots.begin() + "/" };
    const std::string prefix { root + "lib/clang/" };

    std::set<std::string> majors;
    for (const auto& item : entries) {
        if (!item.path.starts_with(prefix)) continue;
        // trim_clangd.py: `name.count("/") >= 4`, on the full (unstripped) archive path.
        if (static_cast<std::size_t>(std::ranges::count(item.path, '/')) < 4) continue;
        const std::string_view relative { std::string_view { item.path }.substr(root.size()) };   // "lib/clang/<major>/..."
        const auto afterLib = relative.find('/');
        const auto afterClang = relative.find('/', afterLib + 1);
        const auto afterMajor = relative.find('/', afterClang + 1);
        majors.insert(std::string { relative.substr(afterClang + 1, afterMajor - afterClang - 1) });
    }
    if (majors.size() != 1) {
        std::vector<std::string> found { majors.begin(), majors.end() };
        return base::fail("clangd-shape", std::format("expected one lib/clang/<major> in {}, found {}",
                                                       archivePath, found.empty() ? "none" : base::join(found, ", ")));
    }
    return Shape { *majors.begin() };
}

// A tool run to completion; the tail of standard error on a non-zero exit, so a strip or codesign
// failure says what it said rather than just that it failed.
base::Result<std::string> run_tool(const std::string& program, std::vector<std::string> arguments) {
    auto result = platform::run(platform::SpawnOptions { .program = program, .arguments = std::move(arguments) },
                                platform::RunBounds { .hard = std::chrono::minutes { 5 } });
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0 || result->timedOut) {
        return base::fail("clangd-tool", std::format("{} exited {}{}\n{}", program, result->exitCode,
                                                      result->timedOut ? " (timed out)" : "",
                                                      platform::last_lines(result->error, 20)));
    }
    return std::move(result->output);
}

// A missing strip tool is not a failure -- trim_clangd.py leaves the binary with its symbols and
// carries on; a tool that runs and fails is (it may have half-written the binary).
//
// llvm-strip reads every architecture; the system's strip reads only its own. So a binary for
// another architecture (linux-arm64 trimmed on an x64 host) is left with its symbols when
// llvm-strip is absent, rather than failing on a strip that could never have read it.
base::Result<bool> strip_linux(const std::string& binary, bool hostArchitecture) {
    auto tool = env::find_executable("llvm-strip");
    if (!tool && hostArchitecture) tool = env::find_executable("strip");
    if (!tool) {
        base::log::info("trim-clangd: no strip tool on PATH that reads this binary; it keeps its symbols");
        return false;
    }
    if (auto ran = run_tool(*tool, { "--strip-all", binary }); !ran) return std::unexpected { ran.error() };
    return true;
}

base::Result<void> thin_and_strip_macos(const std::string& binary, std::string_view appleArch) {
    if constexpr (mcppls::os::FAMILY != mcppls::os::Family::macos) {
        return base::fail("clangd-host", "a darwin clangd needs a macOS host (lipo, strip -x and codesign)");
    } else {
        auto lipo = env::find_executable("lipo");
        if (!lipo) return base::fail("clangd-tool-missing", "lipo is not on PATH");
        const std::string thin { std::format("{}.{}", binary, appleArch) };
        if (auto ran = run_tool(*lipo, { "-thin", std::string { appleArch }, binary, "-output", thin }); !ran) return std::unexpected { ran.error() };
        std::error_code renamed;
        std::filesystem::rename(thin, binary, renamed);
        if (renamed) return base::fail("clangd-tool", std::format("cannot replace {}: {}", binary, renamed.message()));

        auto strip = env::find_executable("strip");
        if (!strip) return base::fail("clangd-tool-missing", "strip is not on PATH");
        if (auto ran = run_tool(*strip, { "-x", binary }); !ran) return std::unexpected { ran.error() };

        // Modifying a Mach-O invalidates its signature, and arm64 macOS refuses to run an arm64
        // executable without a valid one.
        auto codesign = env::find_executable("codesign");
        if (!codesign) return base::fail("clangd-tool-missing", "codesign is not on PATH");
        if (auto ran = run_tool(*codesign, { "--force", "--sign", "-", binary }); !ran) return std::unexpected { ran.error() };
        return {};
    }
}

std::uint64_t dir_size(std::string_view path) {
    std::uint64_t total { 0 };
    std::error_code ignored;
    for (auto it = std::filesystem::recursive_directory_iterator(std::filesystem::path { path }, ignored);
         !ignored && it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ignored)) total += it->file_size(ignored);
    }
    return total;
}

// The LICENSE.TXT a release archive did not carry, from where the lock says the same text is
// (`license-from`): one member of another entry's archive, fetched and verified like any entry.
base::Result<void> license_from_lock(const lock::Lock& lockData, const std::string& clangdEntry,
                                     const std::string& cacheDirectory, const std::string& outDirectory) {
    const auto from = lockData.licenseFrom.find(clangdEntry);
    if (from == lockData.licenseFrom.end()) return {};
    auto source = lock::entry(lockData, from->second.entry);
    if (!source) return std::unexpected { source.error() };
    auto fetched = fetch::get(*source, cacheDirectory);
    if (!fetched) return std::unexpected { fetched.error() };
    const std::string scratch { base::join_path(outDirectory, ".license-from") };
    const std::string member { from->second.member };
    auto written = archive::extract(*fetched, scratch, [&](std::string_view relative) { return relative == member; });
    if (!written) return std::unexpected { written.error() };
    if (written->empty()) {
        return base::fail("clangd-license", std::format("{} has no {} (lock: {}'s license-from)", *fetched, member, clangdEntry));
    }
    std::error_code moved;
    std::filesystem::rename(base::join_path(scratch, member), base::join_path(outDirectory, "LICENSE.TXT"), moved);
    if (moved) return base::fail("clangd-license", std::format("cannot place {}: {}", member, moved.message()));
    fs::remove_all(scratch);
    return {};
}

} // namespace

base::Result<Result> trim(const Options& options, const lock::Lock& lockData) {
    // The lock names the platforms, whether the archive then comes from the lock or from --zip.
    auto platformEntry = lock::platform(lockData, options.platform);
    if (!platformEntry) return base::fail("clangd-platform", platformEntry.error().message);
    const auto target = targets::parse(options.platform);
    if (!target) return base::fail("clangd-platform", std::format("{} is not an <os>-<arch> platform name", options.platform));
    std::string archivePath;
    if (options.zip && !options.zip->empty()) {
        archivePath = *options.zip;
    } else {
        auto lockEntry = lock::entry(lockData, platformEntry->clangd);
        if (!lockEntry) return std::unexpected { lockEntry.error() };
        auto fetched = fetch::get(*lockEntry, options.cacheDirectory);
        if (!fetched) return std::unexpected { fetched.error() };
        archivePath = *fetched;
    }

    auto entries = archive::list(archivePath);
    if (!entries) return std::unexpected { entries.error() };
    auto shape = shape_of(archivePath, *entries);
    if (!shape) return std::unexpected { shape.error() };

    const std::string exe { std::format("clangd{}", targets::executable_suffix(*target)) };
    const std::string binWanted { "bin/" + exe };
    const std::string includePrefix { std::format("lib/clang/{}/include/", shape->major) };

    if (fs::exists(options.outDirectory)) fs::remove_all(options.outDirectory);
    auto written = archive::extract(archivePath, options.outDirectory, [&](std::string_view relative) {
        return relative == binWanted || relative == "LICENSE.TXT" || relative.starts_with(includePrefix);
    });
    if (!written) return std::unexpected { written.error() };
    if (!fs::is_regular_file(base::join_path(options.outDirectory, "LICENSE.TXT"))) {
        if (auto placed = license_from_lock(lockData, platformEntry->clangd, options.cacheDirectory, options.outDirectory); !placed) {
            return std::unexpected { placed.error() };
        }
    }

    const std::string binary { base::join_path(options.outDirectory, binWanted) };
    if (!fs::is_regular_file(binary)) {
        return base::fail("clangd-missing-binary", std::format("{} has no {}", archivePath, binWanted));
    }
    if (target->os != targets::Os::win32) {
        const std::vector<std::string> executables { binary };
        if (auto marked = fs::make_executable(executables); !marked) return std::unexpected { marked.error() };
    }

    bool stripped { false };
    if (options.strip) {
        if (target->os == targets::Os::linux) {
            auto ran = strip_linux(binary, options.platform == mcppls::os::PLATFORM);
            if (!ran) return std::unexpected { ran.error() };
            stripped = *ran;
        } else if (target->os == targets::Os::darwin) {
            if (auto ok = thin_and_strip_macos(binary, targets::apple_arch(target->arch)); !ok) return std::unexpected { ok.error() };
            stripped = true;
        }
    }

    // trim_clangd.py's `host_runs_it`: this host's PLATFORM already folds in family and
    // architecture, so a match here is exactly "this binary can run on this machine".
    std::optional<std::string> versionLine;
    if (options.platform == mcppls::os::PLATFORM) {
        auto out = run_tool(binary, { "--version" });
        if (!out) return std::unexpected { out.error() };
        const auto lines = base::split_lines(*out);
        if (!lines.empty()) versionLine = std::string { lines.front() };
    }

    return Result {
        .directory = fs::canonical_path(options.outDirectory),
        .binary = binary,
        .clangMajor = shape->major,
        .filesKept = written->size(),
        .totalBytes = dir_size(options.outDirectory),
        .stripped = stripped,
        .versionLine = versionLine,
    };
}

} // namespace mcppls::pack::clangd
