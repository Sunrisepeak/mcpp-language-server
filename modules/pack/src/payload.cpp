module mcppls.pack.payload;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.engineidentity;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.version;
import mcppls.pack.fetch;
import mcppls.pack.lock;
import mcppls.pack.targets;
import mcppls.pack.kit;
import mcppls.pack.openkal;
import mcpplibs.os;
import mcppls.base.payload;
import mcppls.toolrun;

namespace os = mcpplibs::os;
namespace mcppls::pack::payload {
namespace {

std::string absolute_of(std::string_view path) {
    if (base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(base::current_directory(), path);
}

// shutil.copy2 (what assemble_payload.py's shutil.copytree uses for the clangd and kit trees)
// preserves the source file's exact mode; copy_file's freshly created destination gets this
// process's create mode instead, and it stays that way -- measured: asking openkal-musl to
// narrow it back to the source's 644 (read-write for the owner, read-only for group and other)
// reports success but leaves it at 664 (this process's 002 umask). fs::make_executable's own
// comment is the same fact from the other side: the read and write triples openkal-musl can
// report back are each all-set or all-clear, so 644's asymmetric write triple (owner writable,
// group and other not) is not a shape a chmod here can actually produce, silently or not. This
// is the one difference a file-by-file comparison against the Python payload finds in the non
// executable files: content (sha256) and the executable bit are unaffected either way.
base::Result<void> copy_one(const std::string& from, const std::string& to) {
    std::error_code failed;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, failed);
    if (failed) return base::fail("payload-copy", std::format("cannot copy {} to {}: {}", from, to, failed.message()));
    return {};
}

// shutil.copytree(src, dst, ignore=shutil.ignore_patterns(excludeName)) -- content only. The one
// executable either tree carries (the clangd binary) gets its bit set explicitly afterward, just
// as assemble_payload.py does; nothing else in either tree needs to be.
base::Result<void> copy_tree(const std::string& src, const std::string& dst, std::string_view excludeName) {
    std::error_code failed;
    auto it = std::filesystem::recursive_directory_iterator(src, failed);
    if (failed) return base::fail("payload-copy", std::format("cannot list {}: {}", src, failed.message()));
    for (; it != std::filesystem::recursive_directory_iterator(); it.increment(failed)) {
        if (failed) return base::fail("payload-copy", std::format("cannot list {}: {}", src, failed.message()));
        const auto& entryPath = it->path();
        const std::string relative { std::filesystem::relative(entryPath, src).generic_string() };
        const std::string destination { base::join_path(dst, relative) };
        if (it->is_directory(failed)) {
            if (std::error_code failed; !std::filesystem::create_directories(destination, failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", destination, failed.message()));
            continue;
        }
        if (!excludeName.empty() && entryPath.filename().string() == excludeName) continue;
        if (std::error_code failed; !std::filesystem::create_directories(base::parent_path(destination), failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", base::parent_path(destination), failed.message()));
        if (auto copied = copy_one(entryPath.string(), destination); !copied) return copied;
    }
    return {};
}

// The --from copy: assemble_payload.py's shutil.copytree(source, out, symlinks=True), which keeps
// a symlink as a symlink and preserves every file's mode (shutil.copy2 -> copystat -> chmod). A
// symlink is recreated with the same target rather than followed; a regular file's executable
// bits are reapplied explicitly afterward, because base::make_executable is the only
// thing here that is allowed to promise a mode landed (openkal-musl's all-or-nothing report).
base::Result<void> copy_tree_preserving_modes(const std::string& src, const std::string& dst) {
    std::error_code failed;
    auto it = std::filesystem::recursive_directory_iterator(src, failed);
    if (failed) return base::fail("payload-copy", std::format("cannot list {}: {}", src, failed.message()));
    std::vector<std::string> executables;
    for (; it != std::filesystem::recursive_directory_iterator(); it.increment(failed)) {
        if (failed) return base::fail("payload-copy", std::format("cannot list {}: {}", src, failed.message()));
        const auto& entryPath = it->path();
        const std::string relative { std::filesystem::relative(entryPath, src).generic_string() };
        const std::string destination { base::join_path(dst, relative) };
        if (it->is_symlink(failed)) {
            const auto target = std::filesystem::read_symlink(entryPath, failed);
            if (failed) return base::fail("payload-copy", std::format("cannot read link {}: {}", entryPath.string(), failed.message()));
            if (std::error_code failed; !std::filesystem::create_directories(base::parent_path(destination), failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", base::parent_path(destination), failed.message()));
            std::filesystem::remove(destination, failed);
            std::filesystem::create_symlink(target, destination, failed);
            if (failed) return base::fail("payload-copy", std::format("cannot link {}: {}", destination, failed.message()));
            continue;
        }
        if (it->is_directory(failed)) {
            if (std::error_code failed; !std::filesystem::create_directories(destination, failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", destination, failed.message()));
            continue;
        }
        if (std::error_code failed; !std::filesystem::create_directories(base::parent_path(destination), failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", base::parent_path(destination), failed.message()));
        if (auto copied = copy_one(entryPath.string(), destination); !copied) return copied;
        const auto permissions = std::filesystem::status(entryPath, failed).permissions();
        if (!failed && (permissions & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                       std::filesystem::perms::others_exec)) != std::filesystem::perms::none) {
            executables.push_back(destination);
        }
    }
    if (!executables.empty()) {
        if (auto marked = base::make_executable(executables); !marked) return std::unexpected { marked.error() };
    }
    return {};
}

std::string git_output(const std::string& root, std::vector<std::string> arguments) {
    auto git = toolrun::find_program("git");
    if (!git) return {};
    auto result = os::process::run(os::process::spawn_options { .program = *git, .args = std::move(arguments), .cwd = root, .bound_lifetime = true },
                                mcpplibs::os::process::bounds { .hard = std::chrono::seconds { 10 } });
    if (!result || !result->status.success()) return {};
    std::string text { result->out };
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

// How this payload was built, recorded where the server can report it back (see
// assemble_payload.py's provenance() docstring for why: a bug report needs to distinguish a
// release build from a from-source one, which a version string alone cannot do between releases).
nlohmann::json provenance(const std::string& root) {
    nlohmann::json record;
    record["source"] = os::env::get("MCPPLS_BUILD_SOURCE").value_or(std::string { "from-source" });
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    record["builtAt"] = std::format("{:%FT%TZ}", now);
    if (const std::string commit { git_output(root, { "rev-parse", "HEAD" }) }; !commit.empty()) {
        record["commit"] = commit;
        // R-2 (plan 0.0.9): whether a tracked file differs from the commit. Files the build itself leaves in the checkout
        // (CI's `cross/` and `host-devtools`) are untracked and say nothing about the source: counting them marked every
        // released payload dirty (0.0.8's included).
        record["dirty"] = !git_output(root, { "status", "--porcelain", "--untracked-files=no" }).empty();
    }
    return record;
}

base::Result<std::string> run_capture(const std::string& program, std::vector<std::string> arguments) {
    auto result = os::process::run(os::process::spawn_options { .program = program, .args = std::move(arguments), .bound_lifetime = true },
                                mcpplibs::os::process::bounds {});
    if (!result) return base::fail("spawn-failed", result.error());
    if (!result->status.success()) {
        return base::fail("payload-tool", std::format("{} exited {}\n{}", program, toolrun::exit_code(result->status),
                                                       base::last_lines(result->err, 20)));
    }
    return std::move(result->out);
}

constexpr std::array<std::string_view, 8> EXECUTABLE_MAGIC {
    "\x7f" "ELF", "MZ", "\xfe\xed\xfa\xce", "\xfe\xed\xfa\xcf",
    "\xce\xfa\xed\xfe", "\xcf\xfa\xed\xfe", "\xca\xfe\xba\xbe", "#!",
};
constexpr std::array<std::string_view, 9> PROGRAM_SUFFIXES {
    ".so", ".dll", ".dylib", ".exe", ".sh", ".bat", ".cmd", ".ps1", ".py",
};

// S4 section 4: what a kit may contain, checked on its files. A walk of the tree, then the rules on kit.json.
void data_only_problems(const std::string& kitDir, std::vector<std::string>& problems) {
    std::error_code failed;
    auto it = std::filesystem::recursive_directory_iterator(kitDir, failed);
    for (; !failed && it != std::filesystem::recursive_directory_iterator(); it.increment(failed)) {
        const auto& entryPath = it->path();
        const std::string relative { std::filesystem::relative(entryPath, kitDir).generic_string() };
        const std::string name { entryPath.filename().string() };
        if (it->is_directory(failed)) {
            // Rule 4: the macOS SDK may not be redistributed.
            if (name.ends_with(".sdk")) problems.push_back(std::format("S4-4-7: the kit contains an SDK directory: {}", relative));
            continue;
        }
        if (name == "SDKSettings.json" || name == "SDKSettings.plist") {
            problems.push_back(std::format("S4-4-7: the kit contains SDK settings: {}", relative));
        }
        std::array<char, 4> head {};
        std::size_t got { 0 };
        if (std::ifstream in { entryPath, std::ios::binary }) {
            in.read(head.data(), static_cast<std::streamsize>(head.size()));
            got = static_cast<std::size_t>(in.gcount());
        }
        const std::string_view headView { head.data(), got };
        const std::string lowerName { base::to_lower_ascii(name) };
        const bool suffixMatch { std::ranges::any_of(PROGRAM_SUFFIXES, [&](std::string_view s) { return lowerName.ends_with(s); }) };
        const bool magicMatch { std::ranges::any_of(EXECUTABLE_MAGIC, [&](std::string_view m) { return headView.starts_with(m); }) };
        // Rule 1: data files only, no executables, shared libraries or scripts.
        if (suffixMatch || magicMatch) {
            problems.push_back(std::format("S4-4-2: the kit contains a program, library or script: {}", relative));
        }
    }
}

// The include directories of the kit's top level and of each of its `targets` (kit-version 2), as kit paths.
std::vector<std::string> include_lists(const nlohmann::json& kit) {
    std::vector<std::string> directories;
    const auto collect = [&](const nlohmann::json& layout) {
        for (const auto& directory : layout.value("system-include-directories", nlohmann::json::array())) {
            if (directory.is_string() && std::ranges::find(directories, directory.get<std::string>()) == directories.end()) {
                directories.push_back(directory.get<std::string>());
            }
        }
    };
    collect(kit);
    for (const auto& item : kit.value("targets", nlohmann::json::object()).items()) {
        if (item.value().is_object()) collect(item.value());
    }
    return directories;
}

// The libc++ version the kit's headers carry: `_LIBCPP_VERSION` of the first `__config` that states it.
std::optional<std::string> headers_libcxx_version(const std::string& kitDir, const nlohmann::json& kit) {
    std::vector<std::string> directories;
    for (const auto& directory : include_lists(kit)) directories.push_back(base::join_path(kitDir, directory));
    return openkal::libcxx_version(directories);
}

std::vector<std::string> module_manifest_problems(const std::string& kitDir, const nlohmann::json& kit) {
    std::error_code ignored;
    std::vector<std::string> problems;
    const std::string metadata { kit.value("stdlib", nlohmann::json::object()).value("module-metadata", std::string {}) };
    const std::string metadataPath { base::join_path(kitDir, metadata) };
    if (!std::filesystem::is_regular_file(metadataPath, ignored)) return { std::format("kit module manifest is missing: {}", metadata) };
    const auto metadataText = os::fs::read(metadataPath);
    const nlohmann::json metadataJson = metadataText ? nlohmann::json::parse(*metadataText, nullptr, false) : nlohmann::json {};
    if (metadataJson.is_discarded() || !metadataJson.is_object()) return { std::format("{} does not parse", metadataPath) };
    const std::string base_ { base::parent_path(metadataPath) };
    std::set<std::string> names;
    for (const auto& module : metadataJson.value("modules", nlohmann::json::array())) {
        const std::string logical { module.value("logical-name", std::string {}) };
        names.insert(logical);
        const std::string sourcePath { module.value("source-path", std::string {}) };
        if (!std::filesystem::is_regular_file(base::join_path(base_, sourcePath), ignored)) {
            problems.push_back(std::format("module {} source is missing: {}", logical, sourcePath));
        }
        const auto localArguments = module.value("local-arguments", nlohmann::json::object());
        for (const auto& directory : localArguments.value("system-include-directories", nlohmann::json::array())) {
            if (directory.is_string() && !std::filesystem::is_directory(base::join_path(base_, directory.get<std::string>()), ignored)) {
                problems.push_back(std::format("module {} include directory is missing: {}", logical, directory.get<std::string>()));
            }
        }
    }
    if (!names.contains("std")) problems.emplace_back("the kit's module manifest does not provide std");
    return problems;
}

// One platform's entry of payload version 5: its runner, its engine and the engine's identity.
void platform_problems(const std::string& dir, std::string_view platform, const nlohmann::json& entry, std::vector<std::string>& problems) {
    std::error_code ignored;
    const auto target = targets::parse(platform);
    if (!target) {
        problems.push_back(std::format("platforms has {}, which is not a platform name", platform));
        return;
    }
    const std::string exe { targets::executable_suffix(*target) };
    const std::string runner { std::format("libexec/okrun/{}/okrun{}", targets::runner_system(*target), exe) };
    const std::string clangd { std::format("clangd/{}/bin/clangd{}", platform, exe) };
    if (entry.value("runner", std::string {}) != runner) {
        problems.push_back(std::format("platforms.{}.runner is {}, expected {}", platform, entry.value("runner", std::string {}), runner));
    }
    const auto engine = entry.value("engines", nlohmann::json::object()).value("clangd", nlohmann::json::object());
    const std::string version { engine.value("version", std::string {}) };
    if (engine.value("path", std::string {}) != clangd) {
        problems.push_back(std::format("platforms.{}.engines.clangd.path is {}, expected {}", platform, engine.value("path", std::string {}), clangd));
    }
    if (version.empty()) problems.push_back(std::format("platforms.{}.engines.clangd.version is empty", platform));
    for (const auto& path : { runner, clangd }) {
        const std::string full { base::join_path(dir, path) };
        if (!std::filesystem::is_regular_file(full, ignored)) {
            problems.push_back(std::format("{} of {} is missing: {}", path == runner ? "the runner" : "clangd", platform, path));
        } else if (exe.empty() && os::host() != os::platform::windows) {
            const auto anyExec = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
            if ((std::filesystem::status(full, ignored).permissions() & anyExec) == std::filesystem::perms::none) {
                problems.push_back(std::format("{} is not executable", path));
            }
        }
    }
    const std::string major { version.substr(0, version.find('.')) };
    if (!std::filesystem::is_regular_file(base::join_path(dir, std::format("clangd/{}/lib/clang/{}/include/stddef.h", platform, major)), ignored)) {
        problems.push_back(std::format("clang builtin headers of {} are missing: clangd/{}/lib/clang/{}/include", platform, platform, major));
    }
    auto identity = base::decode_engine_identity(engine.value("identity", nlohmann::json {}));
    auto digest = fetch::digest_of(base::join_path(dir, clangd));
    if (!identity || !digest) problems.push_back(std::format("the engine of {} lacks a complete identity or readable bytes", platform));
    else if (auto problem = base::engine_identity_problem(*identity, version, platform, *digest)) problems.push_back(std::format("{}: {}", platform, *problem));
    if (!entry.value("files", nlohmann::json::object()).contains(clangd)) {
        problems.push_back(std::format("platforms.{}.files does not list {}", platform, clangd));
    }
}

// Every `files` entry (shared, or one platform's): present, the size and sha256 payload.json says.
void integrity_problems(const std::string& dir, const nlohmann::json& files, std::vector<std::string>& problems) {
    std::error_code ignored;
    for (const auto& item : files.items()) {
        const std::string relative { item.key() };
        const auto& expected = item.value();
        const std::string path { base::join_path(dir, relative) };
        if (!std::filesystem::is_regular_file(path, ignored)) {
            problems.push_back(std::format("an integrity-checked file is missing: {}", relative));
            continue;
        }
        auto digest = fetch::digest_of(path);
        std::error_code sized;
        const auto size = std::filesystem::file_size(path, sized);
        if (!digest || sized) {
            problems.push_back(std::format("cannot check {}", relative));
            continue;
        }
        if (size != expected.value("size", std::uint64_t { 0 })) {
            problems.push_back(std::format("{} is {} bytes, but payload.json says {}", relative, size, expected.value("size", std::uint64_t { 0 })));
        }
        if (*digest != expected.value("sha256", std::string {})) problems.push_back(std::format("{} sha256 does not match payload.json", relative));
    }
}

nlohmann::json integrity_entry(const std::string& path) {
    auto digest = fetch::digest_of(path);
    std::error_code sized;
    const auto size = std::filesystem::file_size(path, sized);
    if (!digest || sized) return nullptr;
    return { { "size", size }, { "sha256", *digest } };
}

// The `<os>-<arch>` the server tree runs on by itself (mcpp-package.json `runs_on.self`); nullopt when the tree was
// packed before `runs_on` was written, and then its runners say it.
std::optional<std::set<std::string>> tree_runs_on(const std::string& tree) {
    const auto text = os::fs::read(base::join_path(tree, "mcpp-package.json"));
    if (!text) return std::nullopt;
    const auto package = nlohmann::json::parse(*text, nullptr, false);
    if (!package.is_object() || !package.contains("runs_on") || !package["runs_on"].is_object()) return std::nullopt;
    std::set<std::string> self;
    for (const auto& pair : package["runs_on"].value("self", nlohmann::json::array())) {
        if (pair.is_string()) self.insert(pair.get<std::string>());
    }
    return self;
}

// What assemble() checks of one platform's engine before anything is written: the binary, and the identity it states.
base::Result<nlohmann::json> engine_identity(const PlatformEngine& engine, const lock::Lock& lockData, bool candidate, std::string& version) {
    std::error_code ignored;
    const auto target = targets::parse(engine.platform);
    const std::string binary { base::join_path(engine.clangdDirectory, std::format("bin/clangd{}", targets::executable_suffix(*target))) };
    if (!std::filesystem::is_regular_file(binary, ignored)) {
        return base::fail("payload-missing", std::format("{} has no bin/clangd{}; is it trimmed for {}?", engine.clangdDirectory,
                                                          targets::executable_suffix(*target), engine.platform));
    }
    if (!std::filesystem::is_regular_file(base::join_path(engine.clangdDirectory, "LICENSE.TXT"), ignored)) {
        return base::fail("payload-missing", std::format("{} has no LICENSE.TXT", engine.clangdDirectory));
    }
    version = lockData.clangdVersion;
    const auto text = os::fs::read(base::join_path(engine.clangdDirectory, "engine.json"));
    if (!text) return base::fail("payload-engine-identity", std::format("the engine of {} has no engine.json with its immutable provenance", engine.platform));
    nlohmann::json identity = nlohmann::json::parse(*text, nullptr, false);
    auto decoded = base::decode_engine_identity(identity);
    auto digest = fetch::digest_of(binary);
    if (!decoded || !digest) return base::fail("payload-engine-identity", std::format("cannot read a complete engine identity and binary SHA of {}", engine.platform));
    const bool maintained { base::llvm_base_version(lockData.clangdVersion) != lockData.clangdVersion };
    if (base::llvm_base_version(decoded->version) != base::llvm_base_version(lockData.clangdVersion)
        || (maintained && decoded->version != lockData.clangdVersion && !candidate)) {
        return base::fail("payload-engine-identity", std::format("the engine of {} does not match the locked LLVM base or fork version", engine.platform));
    }
    version = decoded->version;
    if (auto problem = base::engine_identity_problem(*decoded, version, engine.platform, *digest)) {
        return base::fail("payload-engine-identity", std::format("{}: {}", engine.platform, *problem));
    }
    return identity;
}

} // namespace

std::vector<std::string> verify_kit(std::string_view kitDirectory) {
    std::error_code ignored;
    const std::string kitDir { kitDirectory };
    const std::string kitJson { base::join_path(kitDir, "kit.json") };
    if (!std::filesystem::is_regular_file(kitJson, ignored)) return { std::format("kit manifest is missing: {}", kitJson) };
    const auto text = os::fs::read(kitJson);
    const nlohmann::json kit = text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json {};
    if (kit.is_discarded() || !kit.is_object()) return { std::format("{} does not parse", kitJson) };
    std::vector<std::string> problems;
    const int kitVersion { kit.value("kit-version", -1) };
    if (kitVersion < 1 || kitVersion > kit::KIT_VERSION) problems.push_back(std::format("kit-version is {}, expected 1 to {}", kitVersion, kit::KIT_VERSION));
    for (const auto& directory : include_lists(kit)) {
        if (!std::filesystem::is_directory(base::join_path(kitDir, directory), ignored)) problems.push_back(std::format("kit include directory is missing: {}", directory));
    }
    const auto sysrootField = kit.find("sysroot");
    if (sysrootField != kit.end() && sysrootField->is_string() && !std::filesystem::is_directory(base::join_path(kitDir, sysrootField->get<std::string>()), ignored)) {
        problems.push_back(std::format("kit sysroot is missing: {}", sysrootField->get<std::string>()));
    }
    for (const auto& licenseFile : kit.value("licenses", nlohmann::json::array())) {
        if (licenseFile.is_string() && !std::filesystem::is_regular_file(base::join_path(kitDir, licenseFile.get<std::string>()), ignored)) {
            problems.push_back(std::format("kit license is missing: {}", licenseFile.get<std::string>()));
        }
    }
    auto modules = module_manifest_problems(kitDir, kit);
    problems.insert(problems.end(), modules.begin(), modules.end());
    data_only_problems(kitDir, problems);
    const auto stdlib = kit.value("stdlib", nlohmann::json::object());
    // Rule 3: a libc++ kit's version is the version of the libc++ headers it carries.
    if (stdlib.value("name", std::string {}) == "libc++") {
        const auto carried = headers_libcxx_version(kitDir, kit);
        if (!carried) problems.emplace_back("S4-4-5: libc++ in the kit, but no __config of its headers states _LIBCPP_VERSION");
        else if (*carried != stdlib.value("version", std::string {})) {
            problems.push_back(std::format("S4-4-5: libc++ {} in kit.json, {} in the headers the kit carries", stdlib.value("version", std::string {}), *carried));
        }
    }
    // Rule 4: a macOS kit declares the SDK it needs.
    if (kit.value("target", std::string {}).contains("apple-darwin")) {
        const auto requirements = kit.value("requires", nlohmann::json::array());
        const bool declared { std::ranges::any_of(requirements, [](const nlohmann::json& requirement) {
            return requirement.is_object() && requirement.value("kind", std::string {}) == "macos-sdk";
        }) };
        if (!declared) problems.emplace_back("S4-4-6: a macOS kit does not declare requires macos-sdk");
    }
    return problems;
}

base::Result<std::string> assemble(const AssembleOptions& options, const lock::Lock& lockData) {
    std::error_code ignored;
    if (!std::filesystem::is_regular_file(options.serverImage, ignored)) {
        return base::fail("payload-missing", std::format("the server image {} does not exist", options.serverImage));
    }
    if (options.engines.empty()) return base::fail("payload-platform", "a payload serves at least one platform");
    const auto runsOn = tree_runs_on(options.serverTree);
    std::vector<nlohmann::json> identities;
    std::vector<std::string> versions;
    for (const auto& engine : options.engines) {
        const auto target = targets::parse(engine.platform);
        if (!target) return base::fail("payload-platform", std::format("unknown platform {}", engine.platform));
        if (auto listed = lock::platform(lockData, engine.platform); !listed) return std::unexpected { listed.error() };
        const std::string system { targets::runner_system(*target) };
        const std::string runner { base::join_path(options.serverTree, std::format("libexec/okrun/{}/okrun{}", system, targets::executable_suffix(*target))) };
        if (!std::filesystem::is_regular_file(runner, ignored)) {
            return base::fail("payload-missing", std::format("the server tree {} carries no runner for {} ({}): pack it with --mode self-contained --systems all",
                                                              options.serverTree, system, runner));
        }
        if (runsOn && !runsOn->contains(system)) {
            return base::fail("payload-runs-on", std::format("the server tree does not run on {} by itself (runs_on.self: {})", system,
                                                             base::join(std::vector<std::string> { runsOn->begin(), runsOn->end() }, ", ")));
        }
        if (!std::filesystem::is_directory(engine.clangdDirectory, ignored)) {
            return base::fail("payload-missing", std::format("the engine of {}, {}, does not exist", engine.platform, engine.clangdDirectory));
        }
        std::string version;
        auto identity = engine_identity(engine, lockData, options.candidateEngine, version);
        if (!identity) return std::unexpected { identity.error() };
        identities.push_back(std::move(*identity));
        versions.push_back(std::move(version));
    }
    if (auto problems = verify_kit(options.kitDirectory); !problems.empty()) {
        return base::fail("payload-kit", std::format("the kit {} is not one: {}", options.kitDirectory, base::join(problems, "; ")));
    }
    const auto kitText = os::fs::read(base::join_path(options.kitDirectory, "kit.json"));
    const nlohmann::json kit = nlohmann::json::parse(kitText.value_or("{}"), nullptr, false);

    const std::string out { absolute_of(options.outDirectory) };
    if (std::filesystem::exists(out, ignored)) std::filesystem::remove_all(out, ignored);
    if (std::error_code failed; !std::filesystem::create_directories(base::join_path(out, "bin"), failed) && failed) {
        return base::fail("create-directory", std::format("cannot create {}: {}", base::join_path(out, "bin"), failed.message()));
    }
    if (auto copied = copy_one(options.serverImage, base::join_path(out, "bin/mcppls.okb")); !copied) return std::unexpected { copied.error() };

    nlohmann::json platforms = nlohmann::json::object();
    std::vector<std::string> programs;
    for (std::size_t i { 0 }; i < options.engines.size(); ++i) {
        const auto& engine = options.engines[i];
        const auto target = *targets::parse(engine.platform);
        const std::string exe { targets::executable_suffix(target) };
        const std::string system { targets::runner_system(target) };
        const std::string runnerDirectory { std::format("libexec/okrun/{}", system) };
        if (auto copied = copy_tree_preserving_modes(base::join_path(options.serverTree, runnerDirectory), base::join_path(out, runnerDirectory)); !copied) {
            return std::unexpected { copied.error() };
        }
        const std::string clangdDirectory { std::format("clangd/{}", engine.platform) };
        if (auto copied = copy_tree(engine.clangdDirectory, base::join_path(out, clangdDirectory), "LICENSE.TXT"); !copied) return std::unexpected { copied.error() };
        const std::string runner { std::format("{}/okrun{}", runnerDirectory, exe) };
        const std::string clangd { std::format("{}/bin/clangd{}", clangdDirectory, exe) };
        programs.push_back(base::join_path(out, runner));
        programs.push_back(base::join_path(out, clangd));
        const nlohmann::json integrity = integrity_entry(base::join_path(out, clangd));
        if (integrity.is_null()) return base::fail("payload-stat", std::format("cannot read {}", clangd));
        nlohmann::json entry = nlohmann::json::object();
        entry["runner"] = runner;
        entry["engines"]["clangd"]["version"] = versions[i];
        entry["engines"]["clangd"]["path"] = clangd;
        entry["engines"]["clangd"]["identity"] = identities[i];
        entry["files"][clangd] = integrity;
        platforms[engine.platform] = std::move(entry);
    }
    if (auto marked = base::make_executable(programs); !marked) return std::unexpected { marked.error() };
    if (auto copied = copy_tree(options.kitDirectory, base::join_path(out, "kit"), ""); !copied) return std::unexpected { copied.error() };

    if (std::error_code failed; !std::filesystem::create_directories(base::join_path(out, "licenses"), failed) && failed) {
        return base::fail("create-directory", std::format("cannot create {}: {}", base::join_path(out, "licenses"), failed.message()));
    }
    if (auto copied = copy_one(base::join_path(options.repositoryRoot, "LICENSE"), base::join_path(out, "licenses/mcppls-LICENSE.txt")); !copied) {
        return std::unexpected { copied.error() };
    }
    if (auto copied = copy_one(base::join_path(options.engines.front().clangdDirectory, "LICENSE.TXT"), base::join_path(out, "licenses/LLVM-LICENSE.TXT")); !copied) {
        return std::unexpected { copied.error() };
    }
    // The runner's own license texts, as the tree states them.
    if (const std::string treeLicenses { base::join_path(options.serverTree, "share/licenses") }; std::filesystem::is_directory(treeLicenses, ignored)) {
        if (auto copied = copy_tree(treeLicenses, base::join_path(out, "licenses/server"), ""); !copied) return std::unexpected { copied.error() };
    }

    const nlohmann::json kitIntegrity = integrity_entry(base::join_path(out, "kit/kit.json"));
    const std::string serverVersion { options.serverVersion.value_or(std::string { mcppls::base::VERSION }) };
    nlohmann::json manifest;
    manifest["payload-version"] = PAYLOAD_VERSION;
    manifest["server"]["version"] = serverVersion;
    manifest["server"]["path"] = "bin/mcppls.okb";
    manifest["kit"]["name"] = kit.value("name", std::string {});
    manifest["kit"]["path"] = "kit";
    manifest["platforms"] = platforms;
    manifest["files"]["kit/kit.json"] = kitIntegrity;
    manifest["build"] = provenance(options.repositoryRoot);
    if (auto written = os::fs::write(base::join_path(out, "payload.json"), manifest.dump(2) + "\n"); !written) {
        return base::fail("write-file", written.error());
    }

    // Only the engine this host runs is asked its version: another platform's cannot be started here.
    if (const auto own = platforms.find(std::string { base::payload_platform() }); own != platforms.end()) {
        const std::string engineVersion { (*own)["engines"]["clangd"].value("version", std::string {}) };
        auto reported = run_capture(base::join_path(out, (*own)["engines"]["clangd"].value("path", std::string {})), { "--version" });
        if (!reported) return std::unexpected { reported.error() };
        const std::string_view report { *reported };
        const std::size_t marker { report.find("clangd version ") };
        const std::string_view tail { marker == std::string_view::npos ? std::string_view {} : report.substr(marker + 15) };
        const std::string_view actual { tail.substr(0, tail.find_first_of(" \t\r\n")) };
        if (actual != engineVersion) {
            return base::fail("payload-clangd-version", std::format("clangd reports {}, payload identity says {}", *reported, engineVersion));
        }
    }
    return out;
}

std::vector<std::string> verify(std::string_view payloadDirectory) {
    std::error_code ignored;
    std::vector<std::string> problems;
    const std::string dir { payloadDirectory };
    const std::string manifestPath { base::join_path(dir, "payload.json") };
    if (!std::filesystem::is_regular_file(manifestPath, ignored)) return { std::format("payload.json is missing in {}", dir) };
    auto text = os::fs::read(manifestPath);
    if (!text) return { std::format("cannot read {}: {}", manifestPath, text.error().message()) };
    const nlohmann::json manifest = nlohmann::json::parse(*text, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) return { "payload.json does not parse" };

    if (const int version { manifest.value("payload-version", -1) }; version != PAYLOAD_VERSION) {
        problems.push_back(std::format("payload-version is {}, expected {}", version, PAYLOAD_VERSION));
    }
    const auto server = manifest.value("server", nlohmann::json::object());
    if (server.value("path", std::string {}) != "bin/mcppls.okb") problems.push_back(std::format("server.path is {}, expected bin/mcppls.okb", server.value("path", std::string {})));
    if (server.value("version", std::string {}).empty()) problems.emplace_back("server.version is empty");
    if (!std::filesystem::is_regular_file(base::join_path(dir, "bin/mcppls.okb"), ignored)) problems.emplace_back("the server image is missing: bin/mcppls.okb");

    const auto platforms = manifest.value("platforms", nlohmann::json::object());
    if (platforms.empty()) problems.emplace_back("payload.json serves no platform");
    for (const auto& item : platforms.items()) {
        platform_problems(dir, item.key(), item.value(), problems);
        integrity_problems(dir, item.value().value("files", nlohmann::json::object()), problems);
    }

    const auto kitPart = manifest.value("kit", nlohmann::json::object());
    if (kitPart.value("path", std::string {}) != "kit") problems.push_back(std::format("kit.path is {}, expected 'kit'", kitPart.value("path", std::string {})));
    const auto kitText = os::fs::read(base::join_path(dir, "kit/kit.json"));
    const nlohmann::json kit = kitText ? nlohmann::json::parse(*kitText, nullptr, false) : nlohmann::json {};
    if (kit.is_object() && kit.value("name", std::string {}) != kitPart.value("name", std::string {})) {
        problems.push_back(std::format("kit name {} differs from payload.json {}", kit.value("name", std::string {}), kitPart.value("name", std::string {})));
    }
    for (const auto& problem : verify_kit(base::join_path(dir, "kit"))) problems.push_back(problem);

    for (std::string_view license : { "licenses/mcppls-LICENSE.txt", "licenses/LLVM-LICENSE.TXT" }) {
        if (!std::filesystem::is_regular_file(base::join_path(dir, license), ignored)) problems.push_back(std::format("license is missing: {}", license));
    }
    const auto shared = manifest.value("files", nlohmann::json::object());
    if (!shared.contains("kit/kit.json")) problems.emplace_back("payload.json \"files\" does not list kit/kit.json");
    integrity_problems(dir, shared, problems);

    // A VSIX cannot hold two paths that differ only in case, and neither can the file systems of
    // Windows and macOS.
    std::map<std::string, std::string> seen;
    std::error_code failed;
    auto it = std::filesystem::recursive_directory_iterator(dir, failed);
    for (; !failed && it != std::filesystem::recursive_directory_iterator(); it.increment(failed)) {
        if (!it->is_regular_file(failed)) continue;
        const std::string relative { std::filesystem::relative(it->path(), dir).generic_string() };
        const auto [entryIt, inserted] = seen.try_emplace(base::to_lower_ascii(relative), relative);
        if (!inserted && entryIt->second != relative) problems.push_back(std::format("paths differ only in case: {} and {}", entryIt->second, relative));
    }
    return problems;
}
base::Result<std::string> copy_from(std::string_view source, std::string_view out) {
    std::error_code ignored;
    const std::string sourceAbsolute { absolute_of(source) };
    const std::string outAbsolute { absolute_of(out) };
    if (!std::filesystem::is_regular_file(base::join_path(sourceAbsolute, "payload.json"), ignored)) {
        return base::fail("payload-missing", std::format("{} is not a payload (no payload.json)", sourceAbsolute));
    }
    if (!base::same_path(sourceAbsolute, outAbsolute)) {
        std::filesystem::remove_all(outAbsolute, ignored);
        if (auto copied = copy_tree_preserving_modes(sourceAbsolute, outAbsolute); !copied) return std::unexpected { copied.error() };
    }
    auto problems = verify(outAbsolute);
    if (!problems.empty()) {
        return base::fail("payload-verify", base::join(problems, "; "));
    }
    return outAbsolute;
}

} // namespace mcppls::pack::payload
