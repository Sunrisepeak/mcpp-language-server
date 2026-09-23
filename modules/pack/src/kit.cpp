module mcppls.pack.kit;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.dirs;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.pack.archive;
import mcppls.pack.fetch;
import mcppls.pack.targets;

namespace mcppls::pack::kit {
namespace fs = mcppls::platform::fs;
namespace ar = mcppls::pack::archive;
namespace fetch = mcppls::pack::fetch;

// build_kit.py's SOURCE_SUBTREES, and the phrase its log line joined them with.
constexpr std::array<std::string_view, 6> SOURCE_SUBTREES {
    "runtimes/", "cmake/", "libcxx/", "libcxxabi/", "llvm/cmake/", "llvm/utils/llvm-lit/",
};
constexpr std::string_view SOURCE_SUBTREES_LABEL { "runtimes/, cmake/, libcxx/, libcxxabi/, llvm/cmake/, llvm/utils/llvm-lit/" };

constexpr std::string_view MINGW_TRIPLE { "x86_64-w64-mingw32" };
constexpr std::string_view MINGW_MANIFEST_REL { "x86_64-w64-mingw32/lib/libc++.modules.json" };
constexpr std::string_view MINGW_COPYRIGHT_PREFIX { "x86_64-w64-mingw32/share/mingw32/" };
constexpr std::array<std::string_view, 3> MINGW_EXCLUDED { "*.idl", "*.tlb", "*.def" };

namespace {

// ---- payload.lock.json, read privately for this one recipe table -----------------------------
//
// mcppls.pack.lock (another agent's work, tooling architecture §4) will read the rest of the lock
// file; this reads only the `entries` and `platforms[*].kit` shape build_kit.py used, and stays
// inside this module so the two do not collide before that lands.

struct LockEntry {
    std::string file;
    std::string url;
    std::string sha256;
    std::optional<std::uint64_t> size;
    std::string llvmVersion;   // present only on the llvm-mingw entry
};

struct KitSpec {
    std::string recipe;   // "libcxx-source" | "llvm-mingw"
    std::string source;   // key into Lock::entries
    std::string target;
};

struct Lock {
    std::string libcxxVersion;
    std::map<std::string, LockEntry> entries;
    std::map<std::string, KitSpec> platforms;
};

base::Result<Lock> load_lock(const std::string& path) {
    auto text = fs::read_file(path);
    if (!text) return std::unexpected { text.error() };
    const nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return base::fail("kit-lock", std::format("{} is not valid JSON", path));
    }
    try {
        Lock lock;
        lock.libcxxVersion = document.value("libcxx-version", std::string {});
        for (const auto& item : document.at("entries").items()) {
            const nlohmann::json& value { item.value() };
            LockEntry entry;
            entry.file = value.at("file").get<std::string>();
            entry.url = value.at("url").get<std::string>();
            entry.sha256 = value.at("sha256").get<std::string>();
            if (value.contains("size")) entry.size = value.at("size").get<std::uint64_t>();
            if (value.contains("llvm-version")) entry.llvmVersion = value.at("llvm-version").get<std::string>();
            lock.entries.emplace(item.key(), std::move(entry));
        }
        for (const auto& item : document.at("platforms").items()) {
            const auto& kitValue = item.value().at("kit");
            KitSpec spec;
            spec.recipe = kitValue.at("recipe").get<std::string>();
            spec.source = kitValue.at("source").get<std::string>();
            spec.target = kitValue.at("target").get<std::string>();
            lock.platforms.emplace(item.key(), std::move(spec));
        }
        return lock;
    } catch (const std::exception& error) {
        return base::fail("kit-lock", std::format("{} does not have the shape build_kit.py expects: {}", path, error.what()));
    }
}

// ---- running cmake, ninja, dpkg -- external programs, through the tool run boundary -----------

base::Result<std::string> find_tool(std::string_view envName, std::initializer_list<std::string_view> names) {
    if (auto explicitPath = platform::env::get(envName); explicitPath && !explicitPath->empty()) return *explicitPath;
    for (const auto name : names) {
        if (auto found = platform::env::find_executable(name)) return *found;
    }
    return base::fail("kit-tool", std::format("{} not found on PATH (or set {})", *names.begin(), envName));
}

base::Result<platform::RunResult> execute(const std::string& program, const std::vector<std::string>& arguments,
                                          const std::string& cwd, std::chrono::milliseconds hardBound, bool announce) {
    if (announce) {
        std::vector<std::string> shown { program };
        shown.insert(shown.end(), arguments.begin(), arguments.end());
        base::log::info("{}", base::join(shown, " "));
    }
    return platform::toolrun::run({
        .program = program,
        .arguments = arguments,
        .workDirectory = cwd,
        .purpose = "packaging-kit",
        // The configure step and dpkg -L reach nothing beyond the host; the network is allowed
        // rather than forbidden for the same reason devtools' own step() allows it (tools/devtools/
        // src/common.cpp): this whole path exists to package things, and CMake's standalone
        // runtimes configure is free to look for a system LLVM even though it never needs to here.
        .network = platform::toolrun::Network::allowed,
        .bounds = platform::RunBounds { .hard = hardBound },
    });
}

// build_kit.py's run(): print the command, run it, and stop on a non-zero exit.
base::Result<void> run_tool(const std::string& program, const std::vector<std::string>& arguments, const std::string& cwd) {
    auto result = execute(program, arguments, cwd, std::chrono::hours { 2 }, true);
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0 || result->timedOut) {
        const std::string tail { platform::last_lines(result->error.empty() ? result->output : result->error, 40) };
        return base::fail("kit-run", std::format("{} exited {}{}{}", program, result->exitCode,
                                                 result->timedOut ? " (timed out)" : "",
                                                 tail.empty() ? std::string {} : "\n" + tail));
    }
    return {};
}

// A run captured for its standard output (`dpkg -L`), not its effect.
base::Result<std::string> capture_tool(const std::string& program, const std::vector<std::string>& arguments, const std::string& cwd) {
    auto result = execute(program, arguments, cwd, std::chrono::minutes { 5 }, false);
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0 || result->timedOut) {
        return base::fail("kit-run", std::format("{} exited {}", program, result->exitCode));
    }
    return std::move(result->output);
}

// ---- small path and file helpers ---------------------------------------------------------------

// os.path.relpath(path, base).replace(os.sep, "/") for two paths that are already known to be
// under a common root, which is the only shape this module ever asks for: a lexical diff, not a
// filesystem one -- this repository's own rule for why (mcppls.base.path's header comment) applies
// doubly here, since a kit built for a target other than the host may sit under a preopen the
// build_kit.py port runs inside.
std::string relative_between(std::string_view fromDirectory, std::string_view toPath) {
    auto componentsOf = [](std::string_view path) {
        std::vector<std::string_view> parts;
        for (const auto part : base::split(path, '/')) {
            if (!part.empty()) parts.push_back(part);
        }
        return parts;
    };
    const auto from = componentsOf(fromDirectory);
    const auto to = componentsOf(toPath);
    std::size_t common { 0 };
    while (common < from.size() && common < to.size() && base::same_path(from[common], to[common])) ++common;
    std::string result;
    for (std::size_t i { common }; i < from.size(); ++i) {
        if (!result.empty()) result += '/';
        result += "..";
    }
    for (std::size_t i { common }; i < to.size(); ++i) {
        if (!result.empty()) result += '/';
        result += to[i];
    }
    return result.empty() ? std::string { "." } : result;
}

// A copy of `source`'s contents into `destination` (created if it does not exist), the way
// shutil.copytree(source, destination, dirs_exist_ok=True) does.
base::Result<void> copy_tree(const std::string& source, const std::string& destination) {
    if (auto made = fs::create_directories(destination); !made) return made;
    std::error_code error;
    std::filesystem::copy(std::filesystem::path { source }, std::filesystem::path { destination },
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
    if (error) return base::fail("kit-copy", std::format("cannot copy {} to {}: {}", source, destination, error.message()));
    return {};
}

// build_kit.py's drop_case_collisions(): keeps one file of every group whose paths differ only in
// case, because a kit is unpacked on case-insensitive file systems (Windows, macOS) and shipped
// inside a VSIX, which rejects such names outright.
void drop_case_collisions(const std::string& kitDir) {
    const std::string root { base::normalize_path(kitDir) };
    std::map<std::string, std::vector<std::string>> groups;   // lowercase relative path -> relative paths
    for (const auto& full : fs::list_files(root, {}, {})) {
        auto relative = base::relative_path(full, root);
        if (!relative) continue;
        groups[base::to_lower_ascii(*relative)].push_back(std::move(*relative));
    }
    std::vector<std::string> dropped;
    for (auto& [lower, names] : groups) {
        if (names.size() < 2) continue;
        std::ranges::sort(names, [](const std::string& a, const std::string& b) {
            const auto lowerAlready = [](const std::string& rel) {
                const std::string name { base::file_name(rel) };
                return name == base::to_lower_ascii(name);
            };
            return std::pair { !lowerAlready(a), a } < std::pair { !lowerAlready(b), b };
        });
        for (std::size_t i { 1 }; i < names.size(); ++i) {
            fs::remove_all(base::join_path(root, names[i]));
            dropped.push_back(names[i]);
        }
    }
    if (!dropped.empty()) {
        std::ranges::sort(dropped);
        base::log::info("dropped {} files whose names differ only in case from a kept file: {}", dropped.size(), base::join(dropped, ", "));
    }
}

// ---- the module manifest: every source-path and include directory must resolve inside the kit -

base::Result<std::vector<std::string>> check_manifest(const std::string& kitDir, const std::string& manifestRel) {
    const std::string manifestPath { base::join_path(kitDir, manifestRel) };
    auto text = fs::read_file(manifestPath);
    if (!text) return std::unexpected { text.error() };
    nlohmann::ordered_json manifest = nlohmann::ordered_json::parse(*text, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        return base::fail("kit-manifest", std::format("{} is not valid JSON", manifestPath));
    }
    const std::string manifestDir { base::parent_path(manifestPath) };
    const std::string kitRoot { base::normalize_path(kitDir) };
    bool changed { false };

    // A path that resolves outside the kit is rewritten to the file of the same name under
    // share/libc++/v1 when one exists there; anything else is an error build_kit.py raised too.
    auto relocate = [&](const std::string& value, bool wantDirectory) -> base::Result<std::string> {
        const std::string candidate { base::join_path(manifestDir, value) };
        const bool ok { wantDirectory ? fs::is_directory(candidate) : fs::is_regular_file(candidate) };
        if (ok && base::is_within(candidate, kitRoot)) return value;
        std::string fallback { base::join_path(kitDir, "share/libc++/v1") };
        if (!wantDirectory) fallback = base::join_path(fallback, std::string { base::file_name(value) });
        const bool exists { wantDirectory ? fs::is_directory(fallback) : fs::is_regular_file(fallback) };
        if (!exists) {
            return base::fail("kit-manifest", std::format("manifest {} refers to {}, which is not in the kit", manifestRel, value));
        }
        return relative_between(manifestDir, fallback);
    };

    if (auto modules = manifest.find("modules"); modules != manifest.end() && modules->is_array()) {
        for (auto& module : *modules) {
            if (!module.is_object() || !module.contains("source-path")) continue;
            const std::string sourcePath { module.at("source-path").get<std::string>() };
            auto relocated = relocate(sourcePath, false);
            if (!relocated) return std::unexpected { relocated.error() };
            if (*relocated != sourcePath) {
                module["source-path"] = *relocated;
                changed = true;
            }
            auto local = module.find("local-arguments");
            if (local == module.end() || !local->is_object()) continue;
            auto directories = local->find("system-include-directories");
            if (directories == local->end() || !directories->is_array()) continue;
            for (auto& directory : *directories) {
                if (!directory.is_string()) continue;
                const std::string directoryValue { directory.get<std::string>() };
                auto relocatedDirectory = relocate(directoryValue, true);
                if (!relocatedDirectory) return std::unexpected { relocatedDirectory.error() };
                if (*relocatedDirectory != directoryValue) {
                    directory = *relocatedDirectory;
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        if (auto written = fs::write_file(manifestPath, manifest.dump(2) + "\n"); !written) return std::unexpected { written.error() };
        base::log::info("rewrote paths in {} so they resolve inside the kit", manifestRel);
    }

    std::vector<std::string> names;
    if (auto modules = manifest.find("modules"); modules != manifest.end() && modules->is_array()) {
        for (const auto& module : *modules) {
            if (module.is_object() && module.contains("logical-name") && module.at("logical-name").is_string()) {
                names.push_back(module.at("logical-name").get<std::string>());
            }
        }
    }
    return names;
}

// ---- recipe: libc++ from the LLVM source release (linux-x64, darwin-arm64) ---------------------

base::Result<std::pair<std::vector<std::string>, std::vector<std::string>>> sysroot_headers_from_dpkg() {
    auto dpkg = platform::env::find_executable("dpkg");
    if (!dpkg) return base::fail("kit-dpkg", "dpkg is not available; pass --sysroot-include DIR with the C library headers");
    auto output = capture_tool(*dpkg, { "-L", "libc6-dev", "linux-libc-dev" }, fs::current_directory());
    if (!output) return std::unexpected { output.error() };
    std::vector<std::string> files;
    for (const auto line : base::split_lines(*output)) {
        const std::string path { base::trim(line) };
        if (path.starts_with("/usr/include/") && fs::is_regular_file(path)) files.push_back(path);
    }
    std::vector<std::string> licenses;
    for (const std::string_view candidate : { "/usr/share/doc/libc6-dev/copyright", "/usr/share/doc/linux-libc-dev/copyright" }) {
        if (fs::is_regular_file(candidate)) licenses.emplace_back(candidate);
    }
    return std::pair { std::move(files), std::move(licenses) };
}

base::Result<nlohmann::ordered_json> recipe_libcxx_source(const Options& options, const Lock& lock, const KitSpec& spec,
                                                           const std::string& kitDir, const std::string& workDir) {
    const std::string& target { spec.target };
    const std::string version { lock.libcxxVersion };

    // The two host guards build_kit.py raised SystemExit for: the TARGET platform is data (it
    // comes from --platform, same as every other host), but building it needs facts about the
    // HOST this is running on, which is where mcppls.os's compile-time constant belongs.
    const auto parsed = targets::parse(options.platform);
    if (!parsed) return base::fail("kit-platform", std::format("{} is not an <os>-<arch> platform name", options.platform));
    if (parsed->os == targets::Os::darwin && mcppls::os::FAMILY != mcppls::os::Family::macos) {
        return base::fail("kit-host", std::format("{} configures libc++ for Apple platforms and needs a macOS host", options.platform));
    }
    if (parsed->os == targets::Os::linux && mcppls::os::FAMILY != mcppls::os::Family::linux) {
        return base::fail("kit-host", std::format("{} takes the C library headers from a Linux host", options.platform));
    }
    // dpkg lists the headers of the host's own architecture (/usr/include/<host triple>/...), so
    // without --sysroot-include a Linux kit is built on a host of the architecture it is for.
    if (parsed->os == targets::Os::linux && options.sysrootIncludeDir.empty() && options.platform != mcppls::os::PLATFORM) {
        return base::fail("kit-host", std::format("{} takes its C library headers from dpkg on a {} host, or from --sysroot-include; "
                                                  "this host is {}", options.platform, options.platform, mcppls::os::PLATFORM));
    }

    std::string archive { options.sourceArchive };
    if (archive.empty()) {
        const auto entryIt = lock.entries.find(spec.source);
        if (entryIt == lock.entries.end()) return base::fail("kit-lock", std::format("unknown lock entry {}", spec.source));
        const fetch::Entry entry { .file = entryIt->second.file, .url = entryIt->second.url,
                                   .sha256 = entryIt->second.sha256, .size = entryIt->second.size };
        auto fetched = fetch::get(entry, options.cacheDir);
        if (!fetched) return std::unexpected { fetched.error() };
        archive = *fetched;
    }

    const std::string sourceDir { base::join_path(workDir, "llvm-project") };
    if (!fs::is_regular_file(base::join_path(sourceDir, "runtimes/CMakeLists.txt"))) {
        base::log::info("extracting {} from {}", SOURCE_SUBTREES_LABEL, base::file_name(archive));
        auto extracted = ar::extract(archive, sourceDir, testing::wants_source_subtree);
        if (!extracted) return std::unexpected { extracted.error() };
    }

    auto cmake = find_tool("CMAKE", { "cmake" });
    if (!cmake) return std::unexpected { cmake.error() };
    auto ninja = find_tool("NINJA", { "ninja" });
    if (!ninja) return std::unexpected { ninja.error() };
    auto cc = find_tool("CC", { "clang", "cc", "gcc" });
    if (!cc) return std::unexpected { cc.error() };
    auto cxx = find_tool("CXX", { "clang++", "c++", "g++" });
    if (!cxx) return std::unexpected { cxx.error() };

    const std::string buildDir { base::join_path(workDir, "build") };
    const std::string stageDir { base::join_path(workDir, "stage") };
    fs::remove_all(buildDir);
    fs::remove_all(stageDir);

    const testing::ConfigureInputs configureInputs {
        .platform = options.platform,
        .target = target,
        .runtimesDir = base::join_path(sourceDir, "runtimes"),
        .buildDir = buildDir,
        .stageDir = stageDir,
        .ninja = *ninja,
        .cc = *cc,
        .cxx = *cxx,
    };
    if (auto ran = run_tool(*cmake, testing::configure_arguments(configureInputs), workDir); !ran) return std::unexpected { ran.error() };
    if (auto ran = run_tool(*ninja, testing::install_arguments(buildDir, options.jobs), workDir); !ran) return std::unexpected { ran.error() };

    for (const std::string_view part : { "include", "share", "lib" }) {
        const std::string from { base::join_path(stageDir, part) };
        if (fs::is_directory(from)) {
            if (auto copied = copy_tree(from, base::join_path(kitDir, part)); !copied) return std::unexpected { copied.error() };
        }
    }

    // Every path stored in kit.json below is inside kitDir by construction, so the plain (and
    // always-successful) form of relative_path is enough here; check_manifest's own relocate()
    // is the one that has to cope with a path that might not be.
    auto relative = [&](const std::string& path) { return base::relative_path(path, kitDir).value_or(path); };

    std::vector<std::string> manifests;
    for (const auto& file : fs::list_files(base::join_path(kitDir, "lib"), {}, {})) {
        if (base::file_name(file) == "libc++.modules.json") manifests.push_back(file);
    }
    if (manifests.size() != 1) {
        return base::fail("kit-manifest", std::format("expected one libc++.modules.json in the install, found {}", manifests.size()));
    }

    std::vector<std::string> includeDirectories { "include/c++/v1" };
    const std::string perTarget { base::join_path(kitDir, std::format("include/{}/c++/v1", target)) };
    if (fs::is_directory(perTarget)) {
        includeDirectories.push_back(relative(perTarget));
    } else if (!fs::is_directory(base::join_path(kitDir, "include/c++/v1"))) {
        return base::fail("kit-manifest", "the install has no include/c++/v1");
    }
    const bool haveConfigSite { std::ranges::any_of(includeDirectories, [&](const std::string& directory) {
        return fs::is_regular_file(base::join_path(base::join_path(kitDir, directory), "__config_site"));
    }) };
    if (!haveConfigSite) return base::fail("kit-manifest", "the install has no __config_site");

    const std::string licensesDir { base::join_path(kitDir, "licenses") };
    if (auto made = fs::create_directories(licensesDir); !made) return std::unexpected { made.error() };
    {
        std::error_code copyError;
        std::filesystem::copy_file(std::filesystem::path { base::join_path(sourceDir, "libcxx/LICENSE.TXT") },
                                   std::filesystem::path { base::join_path(licensesDir, "LLVM-LICENSE.TXT") },
                                   std::filesystem::copy_options::overwrite_existing, copyError);
        if (copyError) return base::fail("kit-copy", std::format("cannot copy the LLVM license: {}", copyError.message()));
    }
    std::vector<std::string> licenses { "licenses/LLVM-LICENSE.TXT" };

    nlohmann::ordered_json stdlib;
    stdlib["name"] = "libc++";
    stdlib["version"] = version;
    stdlib["module-metadata"] = relative(manifests.front());

    nlohmann::ordered_json data;
    data["kit-version"] = KIT_VERSION;
    data["name"] = std::format("mcppls-kit-libcxx-{}-{}", version, target);
    data["target"] = target;
    data["stdlib"] = stdlib;
    data["system-include-directories"] = includeDirectories;
    data["sysroot"] = nullptr;
    data["arguments"] = std::vector<std::string> { "-nostdinc++" };

    if (parsed->os == targets::Os::linux) {
        std::vector<std::string> extraLicenses;
        if (!options.sysrootIncludeDir.empty()) {
            const std::string headerRoot { base::join_path(fs::current_directory(), options.sysrootIncludeDir) };
            if (auto copied = copy_tree(headerRoot, base::join_path(kitDir, "sysroot/usr/include")); !copied) {
                return std::unexpected { copied.error() };
            }
            extraLicenses = options.sysrootLicenses;
        } else {
            auto headers = sysroot_headers_from_dpkg();
            if (!headers) return std::unexpected { headers.error() };
            auto& [files, dpkgLicenses] = *headers;
            for (const auto& path : files) {
                const std::string relativeFromRoot { path.starts_with('/') ? path.substr(1) : path };
                const std::string destination { base::join_path(base::join_path(kitDir, "sysroot"), relativeFromRoot) };
                if (auto made = fs::create_directories(base::parent_path(destination)); !made) return std::unexpected { made.error() };
                std::error_code copyError;
                std::filesystem::copy_file(std::filesystem::path { path }, std::filesystem::path { destination },
                                           std::filesystem::copy_options::overwrite_existing, copyError);
                if (copyError) return base::fail("kit-copy", std::format("cannot copy {}: {}", path, copyError.message()));
            }
            base::log::info("copied {} glibc and Linux kernel headers into sysroot/usr/include", files.size());
            extraLicenses = std::move(dpkgLicenses);
        }
        for (std::size_t i { 0 }; i < extraLicenses.size(); ++i) {
            const std::string& path { extraLicenses[i] };
            const std::string baseName { base::file_name(path) };
            std::string name;
            if (baseName == "copyright") {
                const std::string package { base::file_name(base::parent_path(path)) };
                name = std::format("{}-copyright.txt", package);
            } else {
                name = baseName;
            }
            std::error_code copyError;
            std::filesystem::copy_file(std::filesystem::path { path }, std::filesystem::path { base::join_path(licensesDir, name) },
                                       std::filesystem::copy_options::overwrite_existing, copyError);
            if (copyError) return base::fail("kit-copy", std::format("cannot copy license {}: {}", path, copyError.message()));
            licenses.push_back(std::format("licenses/{}", name));
        }
        data["sysroot"] = "sysroot";
        drop_case_collisions(kitDir);
    } else {
        // Apple's SDK license does not allow redistributing the C library headers.
        nlohmann::ordered_json requirement;
        requirement["kind"] = "macos-sdk";
        data["requires"] = nlohmann::ordered_json::array({ requirement });
    }
    data["licenses"] = licenses;

    if (auto written = testing::write_kit_json(kitDir, data); !written) return std::unexpected { written.error() };
    return data;
}

// ---- recipe: llvm-mingw (win32-x64) -------------------------------------------------------------

base::Result<nlohmann::ordered_json> recipe_llvm_mingw(const Options& options, const Lock& lock, const KitSpec& spec,
                                                        const std::string& kitDir) {
    const auto entryIt = lock.entries.find(spec.source);
    if (entryIt == lock.entries.end()) return base::fail("kit-lock", std::format("unknown lock entry {}", spec.source));
    const LockEntry& entry { entryIt->second };
    const std::string version { entry.llvmVersion.empty() ? lock.libcxxVersion : entry.llvmVersion };

    std::string archive { options.sourceArchive };
    if (archive.empty()) {
        const fetch::Entry fetchEntry { .file = entry.file, .url = entry.url, .sha256 = entry.sha256, .size = entry.size };
        auto fetched = fetch::get(fetchEntry, options.cacheDir);
        if (!fetched) return std::unexpected { fetched.error() };
        archive = *fetched;
    }

    const std::string manifestRel { MINGW_MANIFEST_REL };

    base::log::info("extracting headers, module sources and manifest from {}", base::file_name(archive));
    auto extracted = ar::extract(archive, kitDir, testing::wants_mingw_member);
    if (!extracted) return std::unexpected { extracted.error() };

    // Windows and macOS file systems ignore case; names that differ only in case would silently
    // collapse into one when the kit is unpacked there.
    drop_case_collisions(kitDir);

    const std::string licensesDir { base::join_path(kitDir, "licenses") };
    if (auto made = fs::create_directories(licensesDir); !made) return std::unexpected { made.error() };
    {
        std::error_code moveError;
        std::filesystem::rename(std::filesystem::path { base::join_path(kitDir, "LICENSE.TXT") },
                                std::filesystem::path { base::join_path(licensesDir, "LLVM-LICENSE.TXT") }, moveError);
        if (moveError) return base::fail("kit-copy", std::format("cannot move the LLVM license: {}", moveError.message()));
    }
    std::vector<std::string> licenses { "licenses/LLVM-LICENSE.TXT" };

    const std::string copyrightDir { base::join_path(kitDir, MINGW_COPYRIGHT_PREFIX) };
    if (fs::is_directory(copyrightDir)) {
        const std::string mingwLicenses { base::join_path(licensesDir, "mingw-w64") };
        if (auto made = fs::create_directories(mingwLicenses); !made) return std::unexpected { made.error() };
        for (const auto& entryPath : fs::list_directory(copyrightDir)) {
            const std::string name { base::file_name(entryPath) };
            std::error_code moveError;
            std::filesystem::rename(std::filesystem::path { entryPath }, std::filesystem::path { base::join_path(mingwLicenses, name) }, moveError);
            if (moveError) return base::fail("kit-copy", std::format("cannot move {}: {}", entryPath, moveError.message()));
            licenses.push_back(std::format("licenses/mingw-w64/{}", name));
        }
        fs::remove_all(base::join_path(kitDir, std::format("{}/share", MINGW_TRIPLE)));
    }

    nlohmann::ordered_json stdlib;
    stdlib["name"] = "libc++";
    stdlib["version"] = version;
    stdlib["module-metadata"] = manifestRel;

    nlohmann::ordered_json data;
    data["kit-version"] = KIT_VERSION;
    data["name"] = std::format("mcppls-kit-libcxx-{}-{}", version, MINGW_TRIPLE);
    data["target"] = std::string { MINGW_TRIPLE };
    data["stdlib"] = stdlib;
    data["system-include-directories"] = std::vector<std::string> { "generic-w64-mingw32/include/c++/v1", "generic-w64-mingw32/include" };
    data["sysroot"] = nullptr;
    data["arguments"] = std::vector<std::string> { "-nostdinc++", "-nostdlibinc" };
    data["licenses"] = licenses;

    if (auto written = testing::write_kit_json(kitDir, data); !written) return std::unexpected { written.error() };
    return data;
}

} // namespace

namespace testing {

bool wants_source_subtree(std::string_view relativePath) {
    return std::ranges::any_of(SOURCE_SUBTREES, [&](std::string_view prefix) { return relativePath.starts_with(prefix); });
}

// fnmatch.fnmatch-equivalent for MINGW_EXCLUDED's three "*.ext" patterns: '*' and '?' wildcards
// are all build_kit.py ever needed, so that is all this matches.
bool glob_match(std::string_view name, std::string_view pattern) {
    std::size_t n { 0 }, p { 0 }, star { std::string_view::npos }, matched { 0 };
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++n;
            ++p;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            matched = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++matched;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool wants_mingw_member(std::string_view relativePath) {
    if (relativePath.starts_with("generic-w64-mingw32/include/")) {
        const std::string name { base::file_name(relativePath) };
        return !std::ranges::any_of(MINGW_EXCLUDED, [&](std::string_view pattern) { return glob_match(name, pattern); });
    }
    return relativePath.starts_with("share/libc++/v1/") || relativePath == MINGW_MANIFEST_REL || relativePath == "LICENSE.TXT"
        || (relativePath.starts_with(MINGW_COPYRIGHT_PREFIX) && std::string_view { base::file_name(relativePath) }.starts_with("COPYING"));
}

std::vector<std::string> configure_arguments(const ConfigureInputs& inputs) {
    std::vector<std::string> configure {
        "-G", "Ninja", "-S", inputs.runtimesDir, "-B", inputs.buildDir,
        std::format("-DCMAKE_MAKE_PROGRAM={}", inputs.ninja),
        "-DCMAKE_BUILD_TYPE=Release",
        std::format("-DCMAKE_INSTALL_PREFIX={}", inputs.stageDir),
        std::format("-DCMAKE_C_COMPILER={}", inputs.cc),
        std::format("-DCMAKE_CXX_COMPILER={}", inputs.cxx),
        "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi",
        "-DLIBCXXABI_USE_LLVM_UNWINDER=OFF",
        "-DLIBCXX_INSTALL_MODULES=ON",
        "-DLIBCXX_INCLUDE_TESTS=OFF",
        "-DLIBCXX_INCLUDE_BENCHMARKS=OFF",
        "-DLIBCXXABI_INCLUDE_TESTS=OFF",
        "-DLLVM_INCLUDE_TESTS=OFF",
        "-DLLVM_INCLUDE_DOCS=OFF",
    };
    const auto parsed = targets::parse(inputs.platform);
    if (parsed && parsed->os == targets::Os::linux) {
        configure.push_back("-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON");
        configure.push_back(std::format("-DLLVM_DEFAULT_TARGET_TRIPLE={}", inputs.target));
    } else {
        configure.push_back(std::format("-DCMAKE_OSX_ARCHITECTURES={}", targets::apple_arch(parsed ? parsed->arch : targets::Arch::arm64)));
    }
    return configure;
}

std::vector<std::string> install_arguments(const std::string& buildDir, std::optional<int> jobs) {
    std::vector<std::string> arguments { "-C", buildDir };
    if (jobs) {
        arguments.push_back("-j");
        arguments.push_back(std::format("{}", *jobs));
    }
    arguments.push_back("install-cxx-headers");
    arguments.push_back("install-cxxabi-headers");
    arguments.push_back("install-cxx-modules");
    return arguments;
}

base::Result<void> write_kit_json(const std::string& kitDir, nlohmann::ordered_json& data) {
    for (const auto& directory : data.at("system-include-directories")) {
        const std::string value { directory.get<std::string>() };
        if (!fs::is_directory(base::join_path(kitDir, value))) {
            return base::fail("kit-missing", std::format("include directory {} is missing from the kit", value));
        }
    }
    if (const auto& sysroot = data.at("sysroot"); sysroot.is_string() && !sysroot.get<std::string>().empty()) {
        const std::string value { sysroot.get<std::string>() };
        if (!fs::is_directory(base::join_path(kitDir, value))) {
            return base::fail("kit-missing", std::format("sysroot {} is missing from the kit", value));
        }
    }
    for (const auto& license : data.at("licenses")) {
        const std::string value { license.get<std::string>() };
        if (!fs::is_regular_file(base::join_path(kitDir, value))) {
            return base::fail("kit-missing", std::format("license {} is missing from the kit", value));
        }
    }
    auto modules = check_manifest(kitDir, data.at("stdlib").at("module-metadata").get<std::string>());
    if (!modules) return std::unexpected { modules.error() };
    if (std::ranges::find(*modules, std::string { "std" }) == modules->end()) {
        return base::fail("kit-manifest", "the module manifest does not provide std");
    }
    return fs::write_file(base::join_path(kitDir, "kit.json"), data.dump(2) + "\n");
}

} // namespace testing

base::Result<BuiltKit> build(const Options& options) {
    if (!targets::parse(options.platform)) {
        return base::fail("kit-platform", std::format("{} is not an <os>-<arch> platform name", options.platform));
    }
    auto lockResult = load_lock(options.lockPath);
    if (!lockResult) return std::unexpected { lockResult.error() };
    const Lock& lock { *lockResult };
    const auto specIt = lock.platforms.find(options.platform);
    if (specIt == lock.platforms.end()) {
        return base::fail("kit-lock", std::format("{} has no platform {}", options.lockPath, options.platform));
    }
    const KitSpec& spec { specIt->second };

    const std::string kitDir { base::join_path(fs::current_directory(), options.outDir) };
    if (fs::exists(kitDir)) fs::remove_all(kitDir);
    if (auto made = fs::create_directories(kitDir); !made) return std::unexpected { made.error() };

    const bool ownWorkDir { options.workDir.empty() };
    const std::string workDir { ownWorkDir
        ? base::join_path(platform::dirs::temp_directory(), std::format("mcppls-kit-{}", std::random_device {}()))
        : base::join_path(fs::current_directory(), options.workDir) };
    if (auto made = fs::create_directories(workDir); !made) return std::unexpected { made.error() };

    base::Result<nlohmann::ordered_json> data;
    if (spec.recipe == "libcxx-source") {
        data = recipe_libcxx_source(options, lock, spec, kitDir, workDir);
    } else if (spec.recipe == "llvm-mingw") {
        data = recipe_llvm_mingw(options, lock, spec, kitDir);
    } else {
        data = base::fail("kit-lock", std::format("unknown recipe {}", spec.recipe));
    }
    // The scratch work directory is left behind on failure, on purpose: build_kit.py's own
    // cleanup line is never reached when a recipe raises, and a build someone has to debug is
    // exactly the case where the scratch tree is worth keeping.
    if (!data) return std::unexpected { data.error() };

    std::uint64_t totalBytes { 0 }, fileCount { 0 };
    for (const auto& file : fs::list_files(kitDir, {}, {})) {
        if (auto stamp = fs::stamp(file)) {
            totalBytes += stamp->size;
            ++fileCount;
        }
    }
    const std::string name { data->value("name", std::string {}) };
    base::log::info("{}: {} files, {:.1f} MB -> {}", name, fileCount, static_cast<double>(totalBytes) / 1e6, kitDir);

    if (ownWorkDir) fs::remove_all(workDir);

    return BuiltKit { .kitDir = kitDir, .name = name, .fileCount = fileCount, .totalBytes = totalBytes, .manifest = std::move(*data) };
}

} // namespace mcppls::pack::kit
