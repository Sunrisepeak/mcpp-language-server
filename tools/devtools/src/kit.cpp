module mcppls.devtools.kit;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.pack.archive;
import mcppls.pack.kit;
import mcppls.pack.lock;
import mcppls.pack.openkal;
import mcppls.pack.payload;
import mcppls.devtools.common;
import mcpplibs.os;
import mcppls.toolrun;

namespace os = mcpplibs::os;

namespace mcppls::devtools {
namespace {

namespace cmdline = mcpplibs::cmdline;
namespace kit = mcppls::pack::kit;
namespace openkal = mcppls::pack::openkal;
namespace pack = mcppls::pack;

// The openkal targets the kit serves: the `openkal` set of mcpp's target table (SPEC-015).
constexpr std::array<std::string_view, 2> OPENKAL_TARGETS { "x86_64-openkal", "aarch64-openkal" };

// Every value a repeatable option was given, in the order they were given.
std::vector<std::string> repeated(const cmdline::ParsedArgs& arguments, std::string_view name) {
    if (auto option = arguments.option(name)) return option->get().values;
    return {};
}

// `<target>=<file>` of --compile-commands, read.
std::optional<nlohmann::json> given_database(const std::vector<std::string>& given, std::string_view target) {
    for (const auto& item : given) {
        const auto equals = item.find('=');
        if (equals == std::string::npos || std::string_view { item }.substr(0, equals) != target) continue;
        const auto text = os::fs::read(item.substr(equals + 1));
        if (!text) return std::nullopt;
        return nlohmann::json::parse(*text, nullptr, false);
    }
    return std::nullopt;
}

// The version of the platform mcpp selects for the openkal targets (`mcpp self env`), which names the kit.
base::Result<std::string> platform_version(const std::string& mcpp, const std::string& root) {
    auto output = capture(mcpp, { "self", "env", "--target", std::string { OPENKAL_TARGETS.front() }, "--format", "json" }, root, std::chrono::minutes { 10 });
    if (!output) return std::unexpected { output.error() };
    const auto envelope = nlohmann::json::parse(*output, nullptr, false);
    const std::string version { envelope.is_object()
                                    ? envelope.value("data", nlohmann::json::object()).value("platform", nlohmann::json::object()).value("version", std::string {})
                                    : std::string {} };
    if (version.empty()) return base::fail("devtools-kit", "mcpp self env names no platform of the openkal targets");
    return version;
}

// The dev profile's database: a release profile adds NDEBUG, a choice of a project's build and not of the platform.
base::Result<std::string> build_openkal(const std::string& root, const std::string& out, const std::vector<std::string>& given, std::string version) {
    auto mcpp = toolrun::find_program("mcpp");
    if (!mcpp && (given.empty() || version.empty())) return base::fail("devtools-mcpp", "mcpp is not on PATH; pass --compile-commands and --platform-version");
    if (version.empty()) {
        auto read = platform_version(*mcpp, root);
        if (!read) return std::unexpected { read.error() };
        version = *read;
    }
    std::vector<openkal::TargetCommand> commands;
    for (const auto target : OPENKAL_TARGETS) {
        auto database = given_database(given, target);
        if (!database) {
            std::println("  mcpp emit build-database --spec compile-commands --target {} --profile dev", target);
            auto output = capture(*mcpp, { "emit", "build-database", "--spec", "compile-commands", "--target", std::string { target }, "--profile", "dev" },
                                  root, std::chrono::minutes { 30 });
            if (!output) return std::unexpected { output.error() };
            database = nlohmann::json::parse(*output, nullptr, false);
        }
        auto command = openkal::command_of(target, *database, root);
        if (!command) return std::unexpected { command.error() };
        commands.push_back(std::move(*command));
    }
    auto built = openkal::build(openkal::Options { .commands = std::move(commands), .outDir = out, .version = version });
    if (!built) return std::unexpected { built.error() };
    std::println("  kit {}: libc++ {}, {} files, {:.1f} MB", built->name, built->libcxxVersion, built->fileCount, static_cast<double>(built->totalBytes) / 1e6);
    if (auto problems = pack::payload::verify_kit(built->kitDir); !problems.empty()) {
        return base::fail("devtools-kit", std::format("the openkal kit does not verify: {}", base::join(problems, "; ")));
    }
    return built->kitDir;
}

// The name a target kit is published under: `mcppls-kit-<lock kit name>-<libc++ version>`.
std::string target_kit_name(const std::string& root, std::string_view platform, const kit::BuiltKit& built) {
    std::string name { platform };
    if (auto lockData = pack::lock::load(base::join_path(root, "packaging/payload.lock.json"))) {
        if (auto row = pack::lock::platform(*lockData, platform); row && !row->kit.name.empty()) name = row->kit.name;
    }
    const std::string version { built.manifest.value("stdlib", nlohmann::ordered_json::object()).value("version", std::string {}) };
    return std::format("mcppls-kit-{}-{}", name, version);
}

int command_kit(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };
    if (const auto verify = arguments.value("verify"); verify && !verify->empty()) {
        const auto problems = pack::payload::verify_kit(*verify);
        for (const auto& problem : problems) std::println(std::cerr, "mcppls-devtools: problem: {}", problem);
        if (problems.empty()) std::println("kit verified: {}", *verify);
        return problems.empty() ? 0 : 1;
    }
    const auto out = arguments.value("out");
    if (!out || out->empty()) {
        std::println(std::cerr, "mcppls-devtools: --out is required");
        return 2;
    }
    const auto archiveDir = arguments.value("archive");
    std::string kitDir;
    std::string name;
    if (arguments.is_flag_set("openkal")) {
        std::println("mcppls-devtools: building the openkal kit");
        auto built = build_openkal(root, *out, repeated(arguments, "compile-commands"), arguments.value("platform-version").value_or(""));
        if (!built) {
            std::println(std::cerr, "mcppls-devtools: {}", built.error().message);
            return 1;
        }
        kitDir = *built;
        const auto manifest = nlohmann::json::parse(os::fs::read(base::join_path(kitDir, "kit.json")).value_or("{}"), nullptr, false);
        name = manifest.is_object() ? manifest.value("name", std::string { "mcppls-kit-openkal" }) : std::string { "mcppls-kit-openkal" };
    } else {
        const auto platform = arguments.value("platform");
        if (!platform || platform->empty()) {
            std::println(std::cerr, "mcppls-devtools: --openkal, or --platform (one of packaging/payload.lock.json's platforms)");
            return 2;
        }
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
            const auto* end = jobsText->data() + jobsText->size();
            const auto parsed = std::from_chars(jobsText->data(), end, jobs);
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
        kitDir = built->kitDir;
        name = target_kit_name(root, *platform, *built);
        std::println("kit: {} ({} files, {:.1f} MB)", built->kitDir, built->fileCount, static_cast<double>(built->totalBytes) / 1e6);
    }
    if (archiveDir && !archiveDir->empty()) {
        auto archive = kit_archive(kitDir, name, *archiveDir);
        if (!archive) {
            std::println(std::cerr, "mcppls-devtools: {}", archive.error().message);
            return 1;
        }
        std::println("kit archive: {}", *archive);
    }
    return 0;
}

} // namespace

base::Result<std::string> openkal_kit(const std::string& root, const std::string& out) { return build_openkal(root, out, {}, {}); }

base::Result<std::string> kit_archive(const std::string& kitDir, const std::string& name, const std::string& outDir) {
    std::error_code ignored;
    (void)std::filesystem::create_directories(outDir, ignored);
    const std::string archive { base::join_path(outDir, name + ".tar.gz") };
    std::vector<pack::archive::WriteEntry> entries;
    for (const auto& path : base::children(kitDir)) entries.push_back({ path, std::string { base::file_name(path) } });
    if (auto written = pack::archive::write(archive, name, entries); !written) return std::unexpected { written.error() };
    return archive;
}

cmdline::App kit_command(bool& handled, int& status) {
    cmdline::App command { "kit" };
    (void) command.description("Build a semantic kit: the openkal kit a payload carries, or the target kit of one platform; or verify one");
    (void) command.option("openkal").help("The openkal kit, from mcpp's build database of each openkal target");
    (void) command.option("platform").takes_value().help("The target kit of one of packaging/payload.lock.json's platforms, e.g. linux-arm64");
    (void) command.option("out").takes_value().help("kit directory to create (replaced if it exists)");
    (void) command.option("archive").takes_value().value_name("DIR").help("Also write the kit as the release carries it: DIR/mcppls-kit-<target>-<version>.tar.gz");
    (void) command.option("verify").takes_value().value_name("DIR").help("Only check a kit by spec S4's rules");
    (void) command.option("compile-commands").takes_value().multiple().help(
        "--openkal: <target>=FILE, the compile commands of one openkal target instead of asking mcpp (repeatable)");
    (void) command.option("platform-version").takes_value().help("--openkal: the platform version the kit is named by, instead of asking mcpp");
    (void) command.option("cache").takes_value().help("download cache (default: <repository>/.payload-cache)");
    (void) command.option("work").takes_value().help(
        "scratch directory for sources and the configure tree (default: a temporary one, removed on success)");
    (void) command.option("jobs").takes_value().help("parallelism given to ninja's install step");
    (void) command.option("source").takes_value().help("use this archive instead of fetching the lock entry");
    (void) command.option("sysroot-include").takes_value().help(
        "Linux: C library headers to use instead of this host's dpkg packages (needed for another architecture)");
    (void) command.option("sysroot-license").takes_value().multiple().help(
        "Linux: license file for --sysroot-include (repeatable)");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) { handled = true; status = command_kit(arguments); });
    return command;
}

} // namespace mcppls::devtools
