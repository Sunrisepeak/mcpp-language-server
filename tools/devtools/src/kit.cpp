module mcppls.devtools.kit;

import std;
import mcpplibs.cmdline;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.pack.kit;
import mcppls.devtools.common;

namespace mcppls::devtools {
namespace {

namespace cmdline = mcpplibs::cmdline;
namespace kit = mcppls::pack::kit;

// Every value --sysroot-license was given, in the order they were given -- build_kit.py's
// argparse `action="append"` keeps that order too.
std::vector<std::string> repeated(const cmdline::ParsedArgs& arguments, std::string_view name) {
    if (auto option = arguments.option(name)) return option->get().values;
    return {};
}

int command_kit(const cmdline::ParsedArgs& arguments) {
    const auto platform = arguments.value("platform");
    if (!platform || platform->empty()) {
        std::println(std::cerr, "mcppls-devtools: --platform is required (linux-x64, win32-x64 or darwin-arm64)");
        return 2;
    }
    const auto out = arguments.value("out");
    if (!out || out->empty()) {
        std::println(std::cerr, "mcppls-devtools: --out is required");
        return 2;
    }

    const std::string root { repository_root() };
    kit::Options options {
        .platform = *platform,
        .outDir = *out,
        .cacheDir = arguments.value("cache").value_or(base::join_path(root, ".payload-cache")),
        .lockPath = base::join_path(root, "packaging/payload.lock.json"),
        .workDir = arguments.value("work").value_or(""),
        .sourceArchive = arguments.value("source").value_or(""),
        .sysrootIncludeDir = arguments.value("sysroot-include").value_or(""),
        .sysrootLicenses = repeated(arguments, "sysroot-license"),
    };
    if (const auto jobsText = arguments.value("jobs"); jobsText && !jobsText->empty()) {
        int jobs { 0 };
        const auto* begin = jobsText->data();
        const auto* end = jobsText->data() + jobsText->size();
        const auto parsed = std::from_chars(begin, end, jobs);
        if (parsed.ec != std::errc {} || parsed.ptr != end || jobs <= 0) {
            std::println(std::cerr, "mcppls-devtools: --jobs wants a positive number, got {}", *jobsText);
            return 2;
        }
        options.jobs = jobs;
    }

    std::println("mcppls-devtools: building the {} semantic kit", options.platform);
    auto built = kit::build(options);
    if (!built) {
        std::println(std::cerr, "mcppls-devtools: {}", built.error().message);
        return 1;
    }
    std::println("kit: {} ({} files, {:.1f} MB)", built->kitDir, built->fileCount, static_cast<double>(built->totalBytes) / 1e6);
    return 0;
}

} // namespace

cmdline::App kit_command(bool& handled, int& status) {
    cmdline::App command { "kit" };
    (void) command.description("Build the mcppls-kit semantic kit for one platform");
    (void) command.option("platform").takes_value().help("linux-x64 | win32-x64 | darwin-arm64");
    (void) command.option("out").takes_value().help("kit directory to create (replaced if it exists)");
    (void) command.option("cache").takes_value().help("download cache (default: <repository>/.payload-cache)");
    (void) command.option("work").takes_value().help(
        "scratch directory for sources and the configure tree (default: a temporary one, removed on success)");
    (void) command.option("jobs").takes_value().help("parallelism given to ninja's install step");
    (void) command.option("source").takes_value().help("use this archive instead of fetching the lock entry");
    (void) command.option("sysroot-include").takes_value().help(
        "linux-x64: C library headers to use instead of the host's dpkg packages");
    (void) command.option("sysroot-license").takes_value().multiple().help(
        "linux-x64: license file for --sysroot-include (repeatable)");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) { handled = true; status = command_kit(arguments); });
    return command;
}

} // namespace mcppls::devtools
