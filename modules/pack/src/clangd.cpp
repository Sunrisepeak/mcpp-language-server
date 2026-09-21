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
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;

namespace mcppls::pack::clangd {
namespace fs = mcppls::platform::fs;
namespace env = mcppls::platform::env;
namespace platform = mcppls::platform;
namespace {

std::string executable_name(std::string_view platform) { return platform == "win32-x64" ? "clangd.exe" : "clangd"; }

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
base::Result<bool> strip_linux(const std::string& binary) {
    auto tool = env::find_executable("llvm-strip");
    if (!tool) tool = env::find_executable("strip");
    if (!tool) {
        base::log::info("trim-clangd: no strip tool on PATH; the binary keeps its symbols");
        return false;
    }
    if (auto ran = run_tool(*tool, { "--strip-all", binary }); !ran) return std::unexpected { ran.error() };
    return true;
}

base::Result<void> thin_and_strip_macos(const std::string& binary) {
    if constexpr (mcppls::os::FAMILY != mcppls::os::Family::macos) {
        return base::fail("clangd-host", "darwin-arm64 needs a macOS host (lipo, strip -x and codesign)");
    } else {
        auto lipo = env::find_executable("lipo");
        if (!lipo) return base::fail("clangd-tool-missing", "lipo is not on PATH");
        const std::string thin { binary + ".arm64" };
        if (auto ran = run_tool(*lipo, { "-thin", "arm64", binary, "-output", thin }); !ran) return std::unexpected { ran.error() };
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

// trim_clangd.py's argparse `choices=PLATFORMS`: checked independently of whether the archive
// comes from the lock or from --zip.
constexpr std::array<std::string_view, 3> PLATFORMS { "linux-x64", "win32-x64", "darwin-arm64" };

} // namespace

base::Result<Result> trim(const Options& options, const lock::Lock& lockData) {
    if (!std::ranges::any_of(PLATFORMS, [&](std::string_view p) { return p == options.platform; })) {
        return base::fail("clangd-platform", std::format("unknown platform {}", options.platform));
    }
    std::string archivePath;
    if (options.zip && !options.zip->empty()) {
        archivePath = *options.zip;
    } else {
        auto platformEntry = lock::platform(lockData, options.platform);
        if (!platformEntry) return std::unexpected { platformEntry.error() };
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

    const std::string exe { executable_name(options.platform) };
    const std::string binWanted { "bin/" + exe };
    const std::string includePrefix { std::format("lib/clang/{}/include/", shape->major) };

    if (fs::exists(options.outDirectory)) fs::remove_all(options.outDirectory);
    auto written = archive::extract(archivePath, options.outDirectory, [&](std::string_view relative) {
        return relative == binWanted || relative == "LICENSE.TXT" || relative.starts_with(includePrefix);
    });
    if (!written) return std::unexpected { written.error() };

    const std::string binary { base::join_path(options.outDirectory, binWanted) };
    if (!fs::is_regular_file(binary)) {
        return base::fail("clangd-missing-binary", std::format("{} has no {}", archivePath, binWanted));
    }
    if (options.platform != "win32-x64") {
        const std::vector<std::string> executables { binary };
        if (auto marked = fs::make_executable(executables); !marked) return std::unexpected { marked.error() };
    }

    bool stripped { false };
    if (options.strip) {
        if (options.platform == "linux-x64") {
            auto ran = strip_linux(binary);
            if (!ran) return std::unexpected { ran.error() };
            stripped = *ran;
        } else if (options.platform == "darwin-arm64") {
            if (auto ok = thin_and_strip_macos(binary); !ok) return std::unexpected { ok.error() };
            stripped = true;
        }
    }

    // trim_clangd.py's `host_runs_it`: this host's VSCODE_TARGET already folds in family and
    // architecture, so a match here is exactly "this binary can run on this machine".
    std::optional<std::string> versionLine;
    if (options.platform == mcppls::os::VSCODE_TARGET) {
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
