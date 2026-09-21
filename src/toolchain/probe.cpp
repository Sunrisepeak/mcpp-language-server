module mcppls.toolchain.probe;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.spec.database;
import mcppls.toolchain.visualstudio;

namespace mcppls::toolchain {

namespace {

std::string lower_name(std::string_view driverPath) {
    std::string name { base::to_lower_ascii(base::file_name(driverPath)) };
    if (name.ends_with(".exe")) name.resize(name.size() - 4);
    return name;
}

std::string first_line(std::string_view text) {
    const auto lines = base::split_lines(text);
    return lines.empty() ? std::string {} : std::string { base::trim(lines.front()) };
}

base::Result<std::string> query(const Runner& runner, std::string_view driver, std::span<const std::string> relevant,
                                std::initializer_list<std::string_view> extra) {
    std::vector<std::string> argv { std::string { driver } };
    for (const auto& argument : relevant) argv.push_back(argument);
    for (const auto argument : extra) argv.emplace_back(argument);
    auto result = runner(argv);
    if (!result) return std::unexpected { result.error() };
    if (result->timedOut) return base::fail("probe-timeout", std::format("{} did not answer", driver));
    if (result->exitCode != 0) {
        return base::fail("probe-failed", std::format("{} exited with {}: {}", driver, result->exitCode, first_line(result->error)));
    }
    return std::string { base::trim(result->output) };
}

// "clang version 22.1.8 (https://github.com/llvm/llvm-project ca7933e47d3a...)" and friends.
void parse_clang_version(std::string_view text, ToolchainFacts& facts) {
    for (auto line : base::split_lines(text)) {
        line = base::trim(line);
        if (line.starts_with("Configuration file:")) {
            facts.toolchain.configFiles.push_back(base::normalize_path(base::trim(line.substr(19))));
            continue;
        }
        const std::size_t marker { line.find("clang version ") };
        if (marker == std::string_view::npos || !facts.toolchain.version.empty()) continue;
        facts.appleClang = line.find("Apple") != std::string_view::npos;
        std::string_view rest { line.substr(marker + 14) };
        const std::size_t space { rest.find(' ') };
        facts.toolchain.version = std::string { rest.substr(0, space) };
        if (space != std::string_view::npos) {
            const std::string_view tail { rest.substr(space) };
            const std::size_t open { tail.find('(') };
            const std::size_t close { tail.rfind(')') };
            if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
                const auto words = base::split(tail.substr(open + 1, close - open - 1), ' ');
                if (words.size() >= 2) facts.toolchain.buildId = std::string { base::trim(words.back()) };
            }
        }
    }
}

bool is_manifest_answer(std::string_view answer, std::string_view requested) {
    return !answer.empty() && answer != requested && answer != "<NOT PRESENT>" && base::is_absolute_path(answer);
}

std::optional<spec::Stdlib> stdlib_from_manifest(std::string_view manifest) {
    const std::string name { base::to_lower_ascii(base::file_name(manifest)) };
    if (name.starts_with("libc++")) return spec::Stdlib { "libc++", {}, base::normalize_path(manifest) };
    if (name.starts_with("libstdc++")) return spec::Stdlib { "libstdc++", {}, base::normalize_path(manifest) };
    if (name == "modules.json") return spec::Stdlib { "msvc-stl", {}, base::normalize_path(manifest) };
    return spec::Stdlib { "other", {}, base::normalize_path(manifest) };
}

// <prefix>/include/c++/v1 among the arguments -> <prefix>/lib[/<target>]/libc++.modules.json when it exists.
std::optional<std::string> manifest_from_include_paths(std::span<const std::string> arguments, std::string_view target) {
    for (std::size_t i { 0 }; i < arguments.size(); ++i) {
        std::string_view value { arguments[i] };
        if (value == "-isystem" || value == "-I") {
            if (i + 1 >= arguments.size()) break;
            value = arguments[++i];
        } else if (value.starts_with("-isystem")) {
            value.remove_prefix(8);
        } else if (value.starts_with("-I")) {
            value.remove_prefix(2);
        } else {
            continue;
        }
        const std::string directory { base::normalize_path(value) };
        if (!directory.ends_with("/include/c++/v1")) continue;
        const std::string prefix { base::parent_path(base::parent_path(base::parent_path(directory))) };
        std::vector<std::string> candidates;
        if (!target.empty()) candidates.push_back(base::join_path(prefix, std::format("lib/{}/libc++.modules.json", target)));
        candidates.push_back(base::join_path(prefix, "lib/libc++.modules.json"));
        for (const auto& child : platform::fs::list_directory(base::join_path(prefix, "lib"))) {
            candidates.push_back(base::join_path(child, "libc++.modules.json"));
        }
        for (const auto& candidate : candidates) {
            if (platform::fs::is_regular_file(candidate)) return candidate;
        }
    }
    return std::nullopt;
}

// Visual Studio facts for a driver that targets the MSVC ABI: toolset, SDK and the MSVC STL manifest.
void apply_visual_studio(std::string_view driver, const Runner& runner, ToolchainFacts& facts) {
    visualstudio::DiscoveryInputs inputs;
    inputs.environment = [](std::string_view name) { return platform::env::get(name); };
    inputs.run = [&runner](std::span<const std::string> argv) -> std::optional<std::string> {
        auto result = runner(argv);
        if (!result || result->timedOut || result->exitCode != 0) return std::nullopt;
        return result->output;
    };
    // CMake records cl.exe under its short name (C:/PROGRA~1/...); derivations need the long one.
    inputs.recordedDriver = platform::fs::canonical_path(driver);
    const auto installation = visualstudio::discover(inputs);
    if (!installation) {
        facts.problems.push_back("Visual Studio with the C++ tools was not found");
        return;
    }
    facts.msvc = MsvcEnvironment { installation->toolsDirectory, installation->toolsVersion, installation->sdkRoot, installation->sdkVersion };
    const std::string manifest { base::join_path(installation->toolsDirectory, "modules/modules.json") };
    facts.toolchain.stdlib = spec::Stdlib { "msvc-stl", installation->toolsVersion,
                                            platform::fs::is_regular_file(manifest) ? manifest : std::string {} };
    if (facts.msCompatibilityVersion.empty()) facts.msCompatibilityVersion = visualstudio::compatibility_version(installation->toolsVersion);
}

void probe_gcc(std::string_view driver, std::span<const std::string> relevant, const Runner& runner, ToolchainFacts& facts) {
    if (auto target = query(runner, driver, relevant, { "-dumpmachine" })) facts.toolchain.target = *target;
    else facts.problems.push_back(target.error().message);
    if (auto version = query(runner, driver, relevant, { "-dumpfullversion" }); version && !version->empty()) {
        facts.toolchain.version = *version;
    } else if (auto shortVersion = query(runner, driver, relevant, { "-dumpversion" })) {
        facts.toolchain.version = *shortVersion;
    }
    if (auto manifest = query(runner, driver, relevant, { "-print-file-name=libstdc++.modules.json" });
        manifest && is_manifest_answer(*manifest, "libstdc++.modules.json")) {
        facts.toolchain.stdlib = spec::Stdlib { "libstdc++", facts.toolchain.version, base::normalize_path(*manifest) };
    } else {
        facts.toolchain.stdlib = spec::Stdlib { "libstdc++", facts.toolchain.version, {} };
    }
    if (facts.toolchain.target.find("mingw") != std::string::npos) {
        facts.mingwRoot = base::parent_path(base::parent_path(base::normalize_path(driver)));
    } else if (auto libgcc = query(runner, driver, relevant, { "-print-libgcc-file-name" }); libgcc && base::is_absolute_path(*libgcc)) {
        facts.gccInstallDirectory = base::parent_path(base::normalize_path(*libgcc));
    }
}

void probe_clang(std::string_view driver, std::span<const std::string> relevant, const Runner& runner, ToolchainFacts& facts) {
    std::vector<std::string> argv { std::string { driver } };
    for (const auto& argument : relevant) argv.push_back(argument);
    argv.emplace_back("--version");
    if (auto result = runner(argv); result && !result->timedOut) {
        parse_clang_version(result->output, facts);
    } else {
        facts.problems.push_back(result ? "the driver did not answer --version" : result.error().message);
    }
    if (auto target = query(runner, driver, relevant, { "-print-target-triple" })) facts.toolchain.target = *target;
    if (auto resource = query(runner, driver, relevant, { "-print-resource-dir" })) facts.resourceDirectory = base::normalize_path(*resource);
    if (auto manifest = query(runner, driver, relevant, { "-print-library-module-manifest-path" });
        manifest && is_manifest_answer(*manifest, "")) {
        facts.toolchain.stdlib = stdlib_from_manifest(*manifest);
    } else if (auto libcxx = manifest_from_include_paths(relevant, facts.toolchain.target)) {
        facts.toolchain.stdlib = spec::Stdlib { "libc++", {}, *libcxx };
    } else if (auto gnu = query(runner, driver, relevant, { "-print-file-name=libstdc++.modules.json" });
               gnu && is_manifest_answer(*gnu, "libstdc++.modules.json")) {
        facts.toolchain.stdlib = spec::Stdlib { "libstdc++", {}, base::normalize_path(*gnu) };
    }
    if (facts.toolchain.stdlib && facts.toolchain.stdlib->name == "libc++") facts.toolchain.stdlib->version = facts.toolchain.version;
    // clang++ for the MSVC ABI uses the MSVC STL unless a libc++ was selected explicitly (P5).
    if (facts.toolchain.target.find("windows-msvc") != std::string::npos && (!facts.toolchain.stdlib || facts.toolchain.stdlib->name != "libc++")) {
        apply_visual_studio(driver, runner, facts);
    }
}

void probe_msvc(std::string_view driver, const Runner& runner, ToolchainFacts& facts, bool clangCl) {
    const std::string normalized { base::normalize_path(driver) };
    facts.toolchain.target = "x86_64-pc-windows-msvc";
    if (clangCl) {
        std::vector<std::string> argv { normalized, "--version" };
        if (auto result = runner(argv); result && !result->timedOut) {
            parse_clang_version(result->output, facts);
            for (auto line : base::split_lines(result->output)) {
                line = base::trim(line);
                if (line.starts_with("Target:")) facts.toolchain.target = std::string { base::trim(line.substr(7)) };
            }
        }
    } else {
        // cl.exe prints its banner to standard error when started without arguments.
        std::vector<std::string> argv { normalized };
        if (auto result = runner(argv); result && !result->timedOut) {
            const std::string banner { result->error + result->output };
            const std::size_t marker { banner.find("Version ") };
            if (marker != std::string::npos) {
                const std::string_view rest { std::string_view { banner }.substr(marker + 8) };
                facts.toolchain.version = std::string { rest.substr(0, rest.find_first_of(" \r\n")) };
                facts.msCompatibilityVersion = facts.toolchain.version;
            }
            if (banner.find("for ARM64") != std::string::npos) facts.toolchain.target = "aarch64-pc-windows-msvc";
            else if (banner.find("for x86") != std::string::npos) facts.toolchain.target = "i686-pc-windows-msvc";
        }
    }
    apply_visual_studio(normalized, runner, facts);
}

} // namespace

Runner process_runner(std::chrono::milliseconds timeout) {
    return [timeout](std::span<const std::string> argv) -> base::Result<platform::RunResult> {
        if (argv.empty()) return base::fail("probe-command", "empty command");
        // A compiler is an external program like any other: the user's environment, a bound that
        // ends the unit, and a record of what it cost (design 4.2). `sccache` starts a server on
        // its first run and that server inherits the pipe, which is this bound's reason to exist.
        return platform::toolrun::run({
            .program = std::string { argv.front() },
            .arguments = { argv.begin() + 1, argv.end() },
            .purpose = "toolchain",
            .bounds = platform::RunBounds { .hard = timeout },
        });
    };
}

spec::Family classify_driver(std::string_view driverPath) {
    const std::string name { lower_name(driverPath) };
    if (name == "cl") return spec::Family::msvc;
    if (name.find("clang-cl") != std::string::npos) return spec::Family::clang_cl;
    if (name.find("clang") != std::string::npos) return spec::Family::clang;
    // g++, gcc, c++, cc, x86_64-w64-mingw32-g++-16, g++-15 ...
    static constexpr std::array<std::string_view, 4> GNU { "g++", "gcc", "c++", "cc" };
    for (const auto base : GNU) {
        const std::size_t at { name.rfind(base) };
        if (at == std::string::npos) continue;
        const bool startOk { at == 0 || name[at - 1] == '-' };
        std::string_view tail { std::string_view { name }.substr(at + base.size()) };
        const bool tailOk { tail.empty() || (tail.front() == '-' && std::ranges::all_of(tail.substr(1), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) };
        if (startOk && tailOk) return spec::Family::gcc;
    }
    return spec::Family::other;
}

std::vector<std::string> probe_relevant_arguments(std::span<const std::string> arguments) {
    std::vector<std::string> relevant;
    for (std::size_t i { 1 }; i < arguments.size(); ++i) {
        const std::string_view argument { arguments[i] };
        const bool withValue { argument == "-target" || argument == "--target" || argument == "-isysroot" || argument == "--sysroot"
                               || argument == "--gcc-toolchain" || argument == "--config" };
        if (withValue && i + 1 < arguments.size()) {
            relevant.emplace_back(argument);
            relevant.push_back(arguments[++i]);
            continue;
        }
        // A standard library selected by explicit include paths (-nostdinc++ -isystem <prefix>/include/c++/v1).
        const bool includeOption { argument == "-isystem" || argument == "-I" };
        if (includeOption && i + 1 < arguments.size() && base::normalize_path(arguments[i + 1]).ends_with("/c++/v1")) {
            relevant.emplace_back(argument);
            relevant.push_back(arguments[++i]);
            continue;
        }
        if ((argument.starts_with("-isystem") || argument.starts_with("-I")) && base::normalize_path(argument).ends_with("/c++/v1")) {
            relevant.emplace_back(argument);
            continue;
        }
        if (argument.starts_with("--target=") || argument.starts_with("-stdlib=") || argument.starts_with("--sysroot=")
            || argument.starts_with("--gcc-toolchain=") || argument.starts_with("--gcc-install-dir=") || argument.starts_with("--config=")
            || argument == "--no-default-config" || argument == "-m32" || argument == "-m64" || argument == "-nostdinc++") {
            relevant.emplace_back(argument);
        }
    }
    return relevant;
}

base::Result<ToolchainFacts> probe_toolchain(std::string_view driverPath, std::span<const std::string> relevantArguments,
                                             const Runner& runner) {
    ToolchainFacts facts;
    facts.toolchain.driver = base::normalize_path(driverPath);
    facts.toolchain.family = classify_driver(driverPath);
    switch (facts.toolchain.family) {
    case spec::Family::gcc: probe_gcc(driverPath, relevantArguments, runner, facts); break;
    case spec::Family::clang: probe_clang(driverPath, relevantArguments, runner, facts); break;
    case spec::Family::msvc: probe_msvc(driverPath, runner, facts, false); break;
    case spec::Family::clang_cl: probe_msvc(driverPath, runner, facts, true); break;
    case spec::Family::other: return base::fail("toolchain-unknown", std::format("{} is not a recognized compiler driver", driverPath));
    }
    if (facts.toolchain.version.empty() && facts.toolchain.target.empty()) {
        return base::fail("toolchain-not-found", std::format("{} did not answer any query: {}", driverPath,
                                                             facts.problems.empty() ? std::string { "no output" } : facts.problems.front()));
    }
    return facts;
}

std::string toolchain_id(const spec::Toolchain& toolchain) {
    return std::format("{}-{}-{}", spec::to_string(toolchain.family), toolchain.version.empty() ? "unknown" : toolchain.version,
                       toolchain.target.empty() ? "unknown" : toolchain.target);
}

nlohmann::json facts_to_json(const ToolchainFacts& facts) {
    nlohmann::json value = nlohmann::json::object();
    value["family"] = std::string { spec::to_string(facts.toolchain.family) };
    value["version"] = facts.toolchain.version;
    value["build-id"] = facts.toolchain.buildId;
    value["driver"] = facts.toolchain.driver;
    value["target"] = facts.toolchain.target;
    value["config-files"] = facts.toolchain.configFiles;
    if (facts.toolchain.stdlib) {
        value["stdlib"] = nlohmann::json { { "name", facts.toolchain.stdlib->name }, { "version", facts.toolchain.stdlib->version },
                                           { "module-metadata", facts.toolchain.stdlib->moduleMetadata } };
    }
    value["gcc-install-directory"] = facts.gccInstallDirectory;
    value["mingw-root"] = facts.mingwRoot;
    value["resource-directory"] = facts.resourceDirectory;
    value["apple-clang"] = facts.appleClang;
    if (facts.msvc) {
        value["msvc"] = nlohmann::json { { "tools-directory", facts.msvc->toolsDirectory }, { "tools-version", facts.msvc->toolsVersion },
                                         { "sdk-root", facts.msvc->sdkRoot }, { "sdk-version", facts.msvc->sdkVersion } };
    }
    if (!facts.msCompatibilityVersion.empty()) value["ms-compatibility-version"] = facts.msCompatibilityVersion;
    return value;
}

std::optional<ToolchainFacts> facts_from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto family = spec::parse_family(value.value("family", std::string {}));
    if (!family) return std::nullopt;
    ToolchainFacts facts;
    facts.toolchain.family = *family;
    facts.toolchain.version = value.value("version", std::string {});
    facts.toolchain.buildId = value.value("build-id", std::string {});
    facts.toolchain.driver = value.value("driver", std::string {});
    facts.toolchain.target = value.value("target", std::string {});
    for (const auto& file : value.value("config-files", nlohmann::json::array())) {
        if (file.is_string()) facts.toolchain.configFiles.push_back(file.get<std::string>());
    }
    if (const auto stdlib = value.find("stdlib"); stdlib != value.end() && stdlib->is_object()) {
        facts.toolchain.stdlib = spec::Stdlib { stdlib->value("name", std::string {}), stdlib->value("version", std::string {}),
                                                stdlib->value("module-metadata", std::string {}) };
    }
    facts.gccInstallDirectory = value.value("gcc-install-directory", std::string {});
    facts.mingwRoot = value.value("mingw-root", std::string {});
    facts.resourceDirectory = value.value("resource-directory", std::string {});
    facts.appleClang = value.value("apple-clang", false);
    if (const auto msvc = value.find("msvc"); msvc != value.end() && msvc->is_object()) {
        facts.msvc = MsvcEnvironment { msvc->value("tools-directory", std::string {}), msvc->value("tools-version", std::string {}),
                                       msvc->value("sdk-root", std::string {}), msvc->value("sdk-version", std::string {}) };
    }
    facts.msCompatibilityVersion = value.value("ms-compatibility-version", std::string {});
    return facts;
}

ProbeCache::ProbeCache(std::string path) : path_ { std::move(path) } {
    if (auto text = platform::fs::read_file(path_)) {
        nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
        if (!document.is_discarded() && document.is_object()) entries_ = std::move(document);
    }
}

std::string ProbeCache::key_(std::string_view driverPath, std::span<const std::string> relevantArguments) const {
    const auto stamp = platform::fs::stamp(driverPath);
    std::string key { std::format("{}|{}|{}", driverPath, stamp ? stamp->size : 0, stamp ? stamp->modified : 0) };
    for (const auto& argument : relevantArguments) key += "|" + argument;
    return key;
}

std::optional<ToolchainFacts> ProbeCache::find(std::string_view driverPath, std::span<const std::string> relevantArguments) {
    std::lock_guard lock { mutex_ };
    const auto it = entries_.find(key_(driverPath, relevantArguments));
    if (it == entries_.end()) return std::nullopt;
    return facts_from_json(*it);
}

void ProbeCache::store(std::string_view driverPath, std::span<const std::string> relevantArguments, const ToolchainFacts& facts) {
    std::lock_guard lock { mutex_ };
    entries_[key_(driverPath, relevantArguments)] = facts_to_json(facts);
}

void ProbeCache::save() {
    std::lock_guard lock { mutex_ };
    if (path_.empty()) return;
    (void)platform::fs::create_directories(base::parent_path(path_));
    (void)platform::fs::write_file_atomic(path_, entries_.dump(1));
}

base::Result<ToolchainFacts> probe_cached(std::string_view driverPath, std::span<const std::string> relevantArguments,
                                          const Runner& runner, ProbeCache* cache) {
    if (cache != nullptr) {
        if (auto cached = cache->find(driverPath, relevantArguments)) return *cached;
    }
    auto facts = probe_toolchain(driverPath, relevantArguments, runner);
    if (facts && cache != nullptr) cache->store(driverPath, relevantArguments, *facts);
    return facts;
}

} // namespace mcppls::toolchain
