module mcppls.pack.payload;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.version;
import mcppls.os;
import mcppls.pack.fetch;
import mcppls.pack.lock;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;

namespace mcppls::pack::payload {
namespace fs = mcppls::platform::fs;
namespace env = mcppls::platform::env;
namespace platform = mcppls::platform;
namespace {

bool known_platform(std::string_view platform) {
    return std::ranges::any_of(PLATFORMS, [&](std::string_view p) { return p == platform; });
}

std::string suffix(std::string_view platform) { return platform == "win32-x64" ? ".exe" : ""; }

std::string absolute_of(std::string_view path) {
    if (base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(fs::current_directory(), path);
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
            if (auto made = fs::create_directories(destination); !made) return std::unexpected { made.error() };
            continue;
        }
        if (!excludeName.empty() && entryPath.filename().string() == excludeName) continue;
        if (auto made = fs::create_directories(base::parent_path(destination)); !made) return std::unexpected { made.error() };
        if (auto copied = copy_one(entryPath.string(), destination); !copied) return copied;
    }
    return {};
}

// The --from copy: assemble_payload.py's shutil.copytree(source, out, symlinks=True), which keeps
// a symlink as a symlink and preserves every file's mode (shutil.copy2 -> copystat -> chmod). A
// symlink is recreated with the same target rather than followed; a regular file's executable
// bits are reapplied explicitly afterward, because mcppls.platform.fs::make_executable is the only
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
            if (auto made = fs::create_directories(base::parent_path(destination)); !made) return std::unexpected { made.error() };
            std::filesystem::remove(destination, failed);
            std::filesystem::create_symlink(target, destination, failed);
            if (failed) return base::fail("payload-copy", std::format("cannot link {}: {}", destination, failed.message()));
            continue;
        }
        if (it->is_directory(failed)) {
            if (auto made = fs::create_directories(destination); !made) return std::unexpected { made.error() };
            continue;
        }
        if (auto made = fs::create_directories(base::parent_path(destination)); !made) return std::unexpected { made.error() };
        if (auto copied = copy_one(entryPath.string(), destination); !copied) return copied;
        const auto permissions = std::filesystem::status(entryPath, failed).permissions();
        if (!failed && (permissions & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                       std::filesystem::perms::others_exec)) != std::filesystem::perms::none) {
            executables.push_back(destination);
        }
    }
    if (!executables.empty()) {
        if (auto marked = fs::make_executable(executables); !marked) return std::unexpected { marked.error() };
    }
    return {};
}

std::string git_output(const std::string& root, std::vector<std::string> arguments) {
    auto git = env::find_executable("git");
    if (!git) return {};
    auto result = platform::run(platform::SpawnOptions { .program = *git, .arguments = std::move(arguments), .workDirectory = root },
                                platform::RunBounds { .hard = std::chrono::seconds { 10 } });
    if (!result || result->exitCode != 0) return {};
    std::string text { result->output };
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

// How this payload was built, recorded where the server can report it back (see
// assemble_payload.py's provenance() docstring for why: a bug report needs to distinguish a
// release build from a from-source one, which a version string alone cannot do between releases).
nlohmann::json provenance(const std::string& root) {
    nlohmann::json record;
    record["source"] = env::get("MCPPLS_BUILD_SOURCE").value_or(std::string { "from-source" });
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    record["builtAt"] = std::format("{:%FT%TZ}", now);
    if (const std::string commit { git_output(root, { "rev-parse", "HEAD" }) }; !commit.empty()) {
        record["commit"] = commit;
        record["dirty"] = !git_output(root, { "status", "--porcelain" }).empty();
    }
    return record;
}

base::Result<std::string> run_capture(const std::string& program, std::vector<std::string> arguments) {
    auto result = platform::run(platform::SpawnOptions { .program = program, .arguments = std::move(arguments) },
                                platform::RunBounds {});
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0) {
        return base::fail("payload-tool", std::format("{} exited {}\n{}", program, result->exitCode,
                                                       platform::last_lines(result->error, 20)));
    }
    return std::move(result->output);
}

constexpr std::array<std::string_view, 8> EXECUTABLE_MAGIC {
    "\x7f" "ELF", "MZ", "\xfe\xed\xfa\xce", "\xfe\xed\xfa\xcf",
    "\xce\xfa\xed\xfe", "\xcf\xfa\xed\xfe", "\xca\xfe\xba\xbe", "#!",
};
constexpr std::array<std::string_view, 9> PROGRAM_SUFFIXES {
    ".so", ".dll", ".dylib", ".exe", ".sh", ".bat", ".cmd", ".ps1", ".py",
};

// S4 section 4: what a kit may contain, checked on the assembled files.
std::vector<std::string> kit_problems(const std::string& kitDir, const nlohmann::json& kit,
                                      std::string_view platformName, const std::string& clangdVersion) {
    std::vector<std::string> problems;
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
    const auto stdlib = kit.value("stdlib", nlohmann::json::object());
    // Rule 3: a libc++ kit describes the headers of the engine it ships with.
    if (stdlib.value("name", std::string {}) == "libc++" && stdlib.value("version", std::string {}) != clangdVersion) {
        problems.push_back(std::format("S4-4-5: libc++ {} in the kit, clangd {} in the payload",
                                       stdlib.value("version", std::string {}), clangdVersion));
    }
    // Rule 4: a macOS kit declares the SDK it needs.
    if (platformName.starts_with("darwin")) {
        bool declared { false };
        for (const auto& requirement : kit.value("requires", nlohmann::json::array())) {
            if (requirement.is_object() && requirement.value("kind", std::string {}) == "macos-sdk") { declared = true; break; }
        }
        if (!declared) problems.emplace_back("S4-4-6: a macOS kit does not declare requires macos-sdk");
    }
    return problems;
}

} // namespace

base::Result<std::string> assemble(const AssembleOptions& options, const lock::Lock& lockData) {
    if (!known_platform(options.platform)) return base::fail("payload-platform", std::format("unknown platform {}", options.platform));
    if (!fs::is_regular_file(options.serverPath)) {
        return base::fail("payload-missing", std::format("--server {} does not exist", options.serverPath));
    }
    if (!fs::is_directory(options.clangdDirectory)) {
        return base::fail("payload-missing", std::format("--clangd {} does not exist", options.clangdDirectory));
    }
    if (!fs::is_directory(options.kitDirectory)) {
        return base::fail("payload-missing", std::format("--kit {} does not exist", options.kitDirectory));
    }
    const std::string kitJsonPath { base::join_path(options.kitDirectory, "kit.json") };
    if (!fs::is_regular_file(kitJsonPath)) {
        return base::fail("payload-missing", std::format("{} has no kit.json", options.kitDirectory));
    }
    nlohmann::json kit;
    {
        auto text = fs::read_file(kitJsonPath);
        if (!text) return std::unexpected { text.error() };
        try {
            kit = nlohmann::json::parse(*text);
        } catch (const std::exception& error) {
            return base::fail("payload-kit-json", std::format("cannot parse {}: {}", kitJsonPath, error.what()));
        }
    }
    const std::string exe { suffix(options.platform) };
    const std::string clangdBinary { base::join_path(options.clangdDirectory, "bin/clangd" + exe) };
    if (!fs::is_regular_file(clangdBinary)) {
        return base::fail("payload-missing", std::format("{} has no bin/clangd{}; is it trimmed for {}?",
                                                          options.clangdDirectory, exe, options.platform));
    }

    const std::string out { absolute_of(options.outDirectory) };
    if (fs::exists(out)) fs::remove_all(out);
    if (auto made = fs::create_directories(base::join_path(out, "bin")); !made) return std::unexpected { made.error() };

    const std::string serverTarget { base::join_path(out, "bin/mcppls" + exe) };
    if (auto copied = copy_one(options.serverPath, serverTarget); !copied) return std::unexpected { copied.error() };
    if (auto marked = fs::make_executable(std::vector<std::string> { serverTarget }); !marked) return std::unexpected { marked.error() };

    if (auto copied = copy_tree(options.clangdDirectory, base::join_path(out, "clangd"), "LICENSE.TXT"); !copied) return std::unexpected { copied.error() };
    if (auto marked = fs::make_executable(std::vector<std::string> { base::join_path(out, "clangd/bin/clangd" + exe) }); !marked) {
        return std::unexpected { marked.error() };
    }
    if (auto copied = copy_tree(options.kitDirectory, base::join_path(out, "kit"), ""); !copied) return std::unexpected { copied.error() };

    if (auto made = fs::create_directories(base::join_path(out, "licenses")); !made) return std::unexpected { made.error() };
    if (auto copied = copy_one(base::join_path(options.repositoryRoot, "LICENSE"), base::join_path(out, "licenses/mcppls-LICENSE.txt")); !copied) {
        return std::unexpected { copied.error() };
    }
    const std::string clangdLicense { base::join_path(options.clangdDirectory, "LICENSE.TXT") };
    if (!fs::is_regular_file(clangdLicense)) {
        return base::fail("payload-missing", std::format("{} has no LICENSE.TXT", options.clangdDirectory));
    }
    if (auto copied = copy_one(clangdLicense, base::join_path(out, "licenses/LLVM-LICENSE.TXT")); !copied) return std::unexpected { copied.error() };

    // usable plan W9.4: the server compares these at startup (cheap: two small reads and, unless
    // a file changed, a cached hash) and reports payload-corrupt on a mismatch.
    nlohmann::json files = nlohmann::json::object();
    for (const std::string relative : { std::string { "clangd/bin/clangd" } + exe, std::string { "kit/kit.json" } }) {
        const std::string path { base::join_path(out, relative) };
        auto digest = fetch::digest_of(path);
        if (!digest) return std::unexpected { digest.error() };
        std::error_code sized;
        const auto size = std::filesystem::file_size(path, sized);
        if (sized) return base::fail("payload-stat", std::format("cannot stat {}: {}", path, sized.message()));
        files[relative] = { { "size", size }, { "sha256", *digest } };
    }

    const std::string serverVersion { options.serverVersion.value_or(std::string { mcppls::base::VERSION }) };
    const std::string kitName { kit.value("name", std::string {}) };
    nlohmann::json kitEntry { { "name", kitName }, { "path", "kit" } };
    nlohmann::json manifest;
    manifest["payload-version"] = PAYLOAD_VERSION;
    manifest["platform"] = options.platform;
    manifest["server"] = { { "version", serverVersion }, { "path", "bin/mcppls" + exe } };
    manifest["clangd"] = { { "version", lockData.clangdVersion }, { "path", "clangd/bin/clangd" + exe } };
    manifest["kit"] = kitEntry;
    manifest["engines"] = { { "clangd", { { "version", lockData.clangdVersion }, { "path", "clangd/bin/clangd" + exe }, { "kit", kitEntry } } } };
    manifest["files"] = files;
    manifest["build"] = provenance(options.repositoryRoot);

    if (auto written = fs::write_file(base::join_path(out, "payload.json"), manifest.dump(2) + "\n"); !written) {
        return std::unexpected { written.error() };
    }

    // Only when this host can actually run what it just assembled: a cross-assembled payload's
    // clangd cannot be started here to ask it, and asking would be the wrong question anyway.
    if (options.platform == mcppls::os::VSCODE_TARGET) {
        auto reported = run_capture(base::join_path(out, "clangd/bin/clangd" + exe), { "--version" });
        if (!reported) return std::unexpected { reported.error() };
        if (!reported->contains(lockData.clangdVersion)) {
            return base::fail("payload-clangd-version", std::format("clangd reports {}, the lock says {}", *reported, lockData.clangdVersion));
        }
    }
    return out;
}

std::vector<std::string> verify(std::string_view payloadDirectory) {
    std::vector<std::string> problems;
    const std::string dir { payloadDirectory };

    auto needFile = [&](std::string_view relative, std::string_view what) -> std::optional<std::string> {
        const std::string path { base::join_path(dir, relative) };
        if (!fs::is_regular_file(path)) {
            problems.push_back(std::format("{} is missing: {}", what, relative));
            return std::nullopt;
        }
        return path;
    };

    const std::string manifestPath { base::join_path(dir, "payload.json") };
    if (!fs::is_regular_file(manifestPath)) return { std::format("payload.json is missing in {}", dir) };
    auto text = fs::read_file(manifestPath);
    if (!text) return { std::format("cannot read {}: {}", manifestPath, text.error().message) };
    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(*text);
    } catch (const std::exception& error) {
        return { std::format("payload.json does not parse: {}", error.what()) };
    }

    if (manifest.value("payload-version", -1) != PAYLOAD_VERSION) {
        problems.push_back(std::format("payload-version is {}, expected {}", manifest.value("payload-version", -1), PAYLOAD_VERSION));
    }
    const std::string platform { manifest.value("platform", std::string {}) };
    if (!known_platform(platform)) {
        problems.push_back(std::format("unknown platform {}", platform));
        return problems;
    }
    const std::string exe { suffix(platform) };

    for (const auto& [part, expected] : std::vector<std::pair<std::string, std::string>> {
             { "server", "bin/mcppls" + exe }, { "clangd", "clangd/bin/clangd" + exe } }) {
        const auto entry = manifest.value(part, nlohmann::json::object());
        const std::string path { entry.value("path", std::string {}) };
        if (path != expected) problems.push_back(std::format("{}.path is {}, expected {}", part, path, expected));
        if (entry.value("version", std::string {}).empty()) problems.push_back(std::format("{}.version is empty", part));
        auto found = needFile(expected, part);
        if (found && platform != "win32-x64") {
            if constexpr (mcppls::os::FAMILY != mcppls::os::Family::windows) {
                const auto permissions = std::filesystem::status(*found).permissions();
                const auto anyExec = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
                if ((permissions & anyExec) == std::filesystem::perms::none) problems.push_back(std::format("{} is not executable", expected));
            }
        }
    }

    const std::string clangdVersion { manifest.value("clangd", nlohmann::json::object()).value("version", std::string { "0" }) };
    const std::string major { clangdVersion.substr(0, clangdVersion.find('.')) };
    needFile(std::format("clangd/lib/clang/{}/include/stddef.h", major), "clang builtin headers");

    const auto engines = manifest.value("engines", nlohmann::json::object());
    const auto clangdEngine = engines.value("clangd", nlohmann::json::object());
    const auto clangdPart = manifest.value("clangd", nlohmann::json::object());
    if (clangdEngine.value("path", std::string {}) != std::format("clangd/bin/clangd{}", exe) ||
        clangdEngine.value("version", std::string {}) != clangdPart.value("version", std::string {})) {
        problems.push_back(std::format("engines.clangd is {}, expected the clangd part's path and version", clangdEngine.dump()));
    }
    const auto kitPart = manifest.value("kit", nlohmann::json::object());
    if (clangdEngine.value("kit", nlohmann::json::object()) != kitPart) {
        problems.emplace_back("engines.clangd.kit differs from the kit part");
    }

    if (kitPart.value("path", std::string {}) != "kit") {
        problems.push_back(std::format("kit.path is {}, expected 'kit'", kitPart.value("path", std::string {})));
    }
    auto kitJsonPath = needFile("kit/kit.json", "kit manifest");
    if (kitJsonPath) {
        auto kitText = fs::read_file(*kitJsonPath);
        nlohmann::json kit;
        bool parsed { false };
        if (kitText) {
            try {
                kit = nlohmann::json::parse(*kitText);
                parsed = true;
            } catch (const std::exception&) {
                problems.push_back(std::format("{} does not parse", *kitJsonPath));
            }
        } else {
            problems.push_back(std::format("cannot read {}: {}", *kitJsonPath, kitText.error().message));
        }
        if (parsed) {
            const std::string kitDir { base::parent_path(*kitJsonPath) };
            if (kit.value("name", std::string {}) != kitPart.value("name", std::string {})) {
                problems.push_back(std::format("kit name {} differs from payload.json {}", kit.value("name", std::string {}),
                                               kitPart.value("name", std::string {})));
            }
            if (kit.value("kit-version", -1) != 1) {
                problems.push_back(std::format("kit-version is {}, expected 1", kit.value("kit-version", -1)));
            }
            for (const auto& directory : kit.value("system-include-directories", nlohmann::json::array())) {
                if (!directory.is_string()) continue;
                if (!fs::is_directory(base::join_path(kitDir, directory.get<std::string>()))) {
                    problems.push_back(std::format("kit include directory is missing: {}", directory.get<std::string>()));
                }
            }
            const auto sysrootField = kit.find("sysroot");
            if (const std::string sysroot { (sysrootField != kit.end() && sysrootField->is_string()) ? sysrootField->get<std::string>() : std::string {} };
                !sysroot.empty()) {
                if (!fs::is_directory(base::join_path(kitDir, sysroot))) {
                    problems.push_back(std::format("kit sysroot is missing: {}", sysroot));
                }
            }
            for (const auto& licenseFile : kit.value("licenses", nlohmann::json::array())) {
                if (!licenseFile.is_string()) continue;
                if (!fs::is_regular_file(base::join_path(kitDir, licenseFile.get<std::string>()))) {
                    problems.push_back(std::format("kit license is missing: {}", licenseFile.get<std::string>()));
                }
            }
            const std::string metadata { kit.value("stdlib", nlohmann::json::object()).value("module-metadata", std::string {}) };
            const std::string metadataPath { base::join_path(kitDir, metadata) };
            if (!fs::is_regular_file(metadataPath)) {
                problems.push_back(std::format("kit module manifest is missing: {}", metadata));
            } else {
                auto metadataText = fs::read_file(metadataPath);
                nlohmann::json metadataJson;
                bool metadataParsed { false };
                if (metadataText) {
                    try {
                        metadataJson = nlohmann::json::parse(*metadataText);
                        metadataParsed = true;
                    } catch (const std::exception&) {
                        problems.push_back(std::format("{} does not parse", metadataPath));
                    }
                }
                if (metadataParsed) {
                    const std::string base_ { base::parent_path(metadataPath) };
                    std::set<std::string> names;
                    for (const auto& module : metadataJson.value("modules", nlohmann::json::array())) {
                        names.insert(module.value("logical-name", std::string {}));
                        const std::string sourcePath { module.value("source-path", std::string {}) };
                        if (!fs::is_regular_file(base::join_path(base_, sourcePath))) {
                            problems.push_back(std::format("module {} source is missing: {}", module.value("logical-name", std::string {}), sourcePath));
                        }
                        const auto localArguments = module.value("local-arguments", nlohmann::json::object());
                        for (const auto& directory : localArguments.value("system-include-directories", nlohmann::json::array())) {
                            if (!directory.is_string()) continue;
                            if (!fs::is_directory(base::join_path(base_, directory.get<std::string>()))) {
                                problems.push_back(std::format("module {} include directory is missing: {}",
                                                               module.value("logical-name", std::string {}), directory.get<std::string>()));
                            }
                        }
                    }
                    if (!names.contains("std")) problems.emplace_back("the kit's module manifest does not provide std");
                }
            }
            const std::string clangdVersionStr { clangdPart.value("version", std::string {}) };
            auto extra = kit_problems(kitDir, kit, platform, clangdVersionStr);
            problems.insert(problems.end(), extra.begin(), extra.end());
        }
    }

    needFile("licenses/mcppls-LICENSE.txt", "license");
    needFile("licenses/LLVM-LICENSE.TXT", "license");

    const auto integrityFiles = manifest.value("files", nlohmann::json::object());
    for (const std::string expected : { std::string { "clangd/bin/clangd" } + exe, std::string { "kit/kit.json" } }) {
        if (!integrityFiles.contains(expected)) problems.push_back(std::format("payload.json \"files\" does not list {}", expected));
    }
    for (const auto& item : integrityFiles.items()) {
        const std::string& relative = item.key();
        const auto& expected = item.value();
        auto path = needFile(relative, "an integrity-checked file");
        if (!path) continue;
        auto digest = fetch::digest_of(*path);
        std::error_code sized;
        const auto size = std::filesystem::file_size(*path, sized);
        if (!digest || sized) {
            problems.push_back(std::format("cannot check {}", relative));
            continue;
        }
        const auto expectedSize = expected.value("size", std::uint64_t { 0 });
        if (size != expectedSize) {
            problems.push_back(std::format("{} is {} bytes, but payload.json says {}", relative, size, expectedSize));
        }
        if (*digest != expected.value("sha256", std::string {})) {
            problems.push_back(std::format("{} sha256 does not match payload.json", relative));
        }
    }

    // A VSIX cannot hold two paths that differ only in case, and neither can the file systems of
    // Windows and macOS.
    std::map<std::string, std::string> seen;
    std::error_code failed;
    auto it = std::filesystem::recursive_directory_iterator(dir, failed);
    for (; !failed && it != std::filesystem::recursive_directory_iterator(); it.increment(failed)) {
        if (!it->is_regular_file(failed)) continue;
        const std::string relative { std::filesystem::relative(it->path(), dir).generic_string() };
        const std::string lowered { base::to_lower_ascii(relative) };
        const auto [entryIt, inserted] = seen.try_emplace(lowered, relative);
        if (!inserted && entryIt->second != relative) {
            problems.push_back(std::format("paths differ only in case: {} and {}", entryIt->second, relative));
        }
    }
    return problems;
}

base::Result<std::string> copy_from(std::string_view source, std::string_view out) {
    const std::string sourceAbsolute { absolute_of(source) };
    const std::string outAbsolute { absolute_of(out) };
    if (!fs::is_regular_file(base::join_path(sourceAbsolute, "payload.json"))) {
        return base::fail("payload-missing", std::format("{} is not a payload (no payload.json)", sourceAbsolute));
    }
    if (!base::same_path(sourceAbsolute, outAbsolute)) {
        fs::remove_all(outAbsolute);
        if (auto copied = copy_tree_preserving_modes(sourceAbsolute, outAbsolute); !copied) return std::unexpected { copied.error() };
    }
    auto problems = verify(outAbsolute);
    if (!problems.empty()) {
        return base::fail("payload-verify", base::join(problems, "; "));
    }
    return outAbsolute;
}

} // namespace mcppls::pack::payload
