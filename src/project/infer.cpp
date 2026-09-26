module mcppls.project.infer;

import std;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.spec.database;
import mcppls.toolchain.probe;
import mcppls.project.compdb;
import mcppls.project.scan;
import mcppls.project.boundary;

namespace mcppls::project {

namespace {

constexpr std::array<std::string_view, 16> SOURCE_EXTENSIONS {
    ".cpp", ".cc", ".cxx", ".c++", ".cppm", ".ccm", ".cxxm", ".c++m", ".ixx", ".mpp", ".mxx", ".cp",
    ".CPP", ".CC", ".CXX", ".C",
};
// Build output, and what package managers install or cache (fix plan F5): their packages' sources are not
// this project's. Issue #23's fallback scan of GalTranslPP took 166 of its 349 units from vcpkg_installed/
// and reported the modules there as ambiguous. list_files skips hidden directories already; they are
// named here too, so the list says everything it leaves out.
constexpr std::array<std::string_view, 14> SKIPPED_DIRECTORIES { "target", "build", "node_modules", "out", "_build", "cmake-build-debug",
                                                                 "cmake-build-release", "vcpkg_installed", "vcpkg", ".conan", ".conan2",
                                                                 ".xmake", ".cache", ".git" };

// A directory below the root with a vcpkg.json of its own is a vcpkg package -- a port, an overlay, a
// vendored library -- and not part of this project (fix plan F5). Each directory is looked at once.
class VcpkgPackages {
public:
    explicit VcpkgPackages(std::string_view root) : rootKey_ { base::path_key(root) } {}

    bool contains(std::string_view file) {
        std::string directory { base::parent_path(file) };
        std::vector<std::string> walked;
        bool found { false };
        while (!directory.empty() && base::path_key(directory) != rootKey_ && base::is_within(directory, rootKey_)) {
            if (const auto known = package_.find(directory); known != package_.end()) {
                found = known->second;
                break;
            }
            walked.push_back(directory);
            if (platform::fs::is_regular_file(base::join_path(directory, "vcpkg.json"))) {
                found = true;
                break;
            }
            const std::string parent { base::parent_path(directory) };
            if (parent == directory) break;
            directory = parent;
        }
        for (auto& visited : walked) package_.insert_or_assign(std::move(visited), found);
        return found;
    }

private:
    std::string rootKey_;
    std::map<std::string, bool, std::less<>> package_;   // directory -> whether it is inside a vcpkg package
};

void fill_modules(spec::TranslationUnit& unit, const ScanResult& scanned) {
    unit.role = role_of(scanned);
    if (const std::string provided { provided_name(scanned) }; !provided.empty()) unit.providedModules.emplace_back(provided, std::string {});
    unit.requiredModules = required_names(scanned);
    unit.isPrivate = unit.role == spec::Role::non_module || unit.role == spec::Role::module_implementation;
}

} // namespace

Scanner file_scanner() {
    return [](std::string_view path) {
        auto text = platform::fs::read_file(path);
        return text ? scan_source(*text) : ScanResult {};
    };
}

InferredDatabase database_from_commands(std::span<const CompileCommand> commands, std::string_view familyName,
                                        const Scanner& scanner, const Prober& prober) {
    InferredDatabase result;
    result.database.hasIde = true;
    result.database.generator = spec::Generator { "mcppls", "compile-commands" };
    std::map<std::string, std::size_t, std::less<>> setByToolchain;
    std::map<std::string, std::string, std::less<>> toolchainByProbeKey;

    for (const auto& command : commands) {
        if (command.arguments.empty()) continue;
        const std::string_view language { base::extension(command.file) };
        if (base::iequals_ascii(language, ".c") || base::iequals_ascii(language, ".s") || base::iequals_ascii(language, ".asm")) continue;
        std::string driver { command.arguments.front() };
        if (!base::is_absolute_path(driver) && driver.find('/') != std::string::npos) driver = base::join_path(command.directory, driver);
        const auto relevant = toolchain::probe_relevant_arguments(command.arguments);
        std::string probeKey { driver };
        for (const auto& argument : relevant) probeKey += "|" + argument;

        std::string toolchainId;
        if (const auto known = toolchainByProbeKey.find(probeKey); known != toolchainByProbeKey.end()) {
            toolchainId = known->second;
        } else {
            if (auto facts = prober(driver, relevant)) {
                toolchainId = toolchain::toolchain_id(facts->toolchain);
                if (!result.facts.contains(toolchainId)) {
                    result.database.toolchains.emplace_back(toolchainId, facts->toolchain);
                    result.facts.emplace(toolchainId, std::move(*facts));
                }
            } else {
                result.problems.push_back(std::format("cannot probe compiler {}", driver));
            }
            toolchainByProbeKey.emplace(probeKey, toolchainId);
        }

        auto setIt = setByToolchain.find(toolchainId);
        if (setIt == setByToolchain.end()) {
            spec::Set set;
            set.name = toolchainId.empty() ? std::string { familyName } : std::format("{}@{}", familyName, toolchainId);
            set.familyName = std::string { familyName };
            set.hasIde = true;
            set.toolchain = toolchainId;
            set.kind = "other";
            result.database.sets.push_back(std::move(set));
            setIt = setByToolchain.emplace(toolchainId, result.database.sets.size() - 1).first;
        }
        spec::TranslationUnit unit;
        unit.source = command.file;
        unit.workDirectory = command.directory;
        unit.arguments = command.arguments;
        unit.object = command.output;
        fill_modules(unit, scanner(command.file));
        result.database.sets[setIt->second].units.push_back(std::move(unit));
    }
    // Every set sees every other: a compile_commands.json has no visibility information.
    for (auto& set : result.database.sets) {
        for (const auto& other : result.database.sets) {
            if (other.name != set.name) set.visibleSets.push_back(other.name);
        }
    }
    return result;
}

InferredDatabase enrich_database(spec::Database database, const Scanner& scanner, const Prober& prober) {
    InferredDatabase result;
    database.hasIde = true;
    if (database.profileVersion.empty()) database.profileVersion = std::string { spec::PROFILE_VERSION };
    // Facts for the toolchains a producer described.
    for (const auto& [id, toolchain] : database.toolchains) {
        if (toolchain.driver.empty()) continue;
        if (auto facts = prober(toolchain.driver, std::vector<std::string> {})) {
            // What the producer stated about the standard library wins over what a bare query says.
            if (toolchain.stdlib && !toolchain.stdlib->moduleMetadata.empty()) facts->toolchain.stdlib = toolchain.stdlib;
            result.facts.emplace(id, std::move(*facts));
        }
    }
    for (auto& set : database.sets) {
        set.hasIde = true;
        if ((set.toolchain.empty() || !result.facts.contains(set.toolchain)) && !set.units.empty()) {
            const auto& first = set.units.front();
            std::string driver { first.arguments.front() };
            if (!base::is_absolute_path(driver)) driver = base::join_path(first.workDirectory, driver);
            const auto relevant = toolchain::probe_relevant_arguments(first.arguments);
            if (auto facts = prober(driver, relevant)) {
                set.toolchain = toolchain::toolchain_id(facts->toolchain);
                if (spec::find_toolchain(database, set.toolchain) == nullptr) database.toolchains.emplace_back(set.toolchain, facts->toolchain);
                result.facts.emplace(set.toolchain, std::move(*facts));
            } else {
                result.problems.push_back(std::format("cannot probe compiler {}", driver));
            }
        }
        for (auto& unit : set.units) {
            // A producer that stated a role also stated provides and requires (S1 level 2).
            if (unit.role && *unit.role != spec::Role::unknown) continue;
            const ScanResult scanned { scanner(spec::absolute_source(unit)) };
            if (!unit.role) unit.role = role_of(scanned);
            if (unit.requiredModules.empty()) unit.requiredModules = required_names(scanned);
            if (unit.providedModules.empty()) {
                if (const std::string provided { provided_name(scanned) }; !provided.empty()) unit.providedModules.emplace_back(provided, std::string {});
            }
        }
    }
    result.database = std::move(database);
    return result;
}

std::string inferred_language_standard(const std::optional<toolchain::ToolchainFacts>& facts) {
    if (!facts) return "c++26";
    const std::string_view version { facts->toolchain.version };
    int major { 0 };
    (void)std::from_chars(version.data(), version.data() + version.size(), major);
    switch (facts->toolchain.family) {
    case spec::Family::gcc: return major >= 14 ? "c++26" : "c++23";
    case spec::Family::clang: return major >= 20 ? "c++26" : major >= 17 ? "c++2c" : "c++23";
    default: return "c++26";   // the MSVC family is given /std:c++latest, which is C++26 to the engine
    }
}

InferredDatabase infer_database(std::string_view rootInput, const InferOptions& options, const Scanner& scanner) {
    InferredDatabase result;
    const std::string root { base::normalize_path(rootInput) };
    result.database.hasIde = true;
    result.database.generator = spec::Generator { "mcppls", "inferred" };

    spec::Set set;
    set.name = "inferred";
    set.familyName = std::string { base::file_name(root) };
    set.hasIde = true;
    set.kind = "other";
    std::string driver { options.engineDriver };
    if (options.facts) {
        set.toolchain = toolchain::toolchain_id(options.facts->toolchain);
        driver = options.facts->toolchain.driver;
        result.database.toolchains.emplace_back(set.toolchain, options.facts->toolchain);
        result.facts.emplace(set.toolchain, *options.facts);
    }
    // cl's own spelling for the MSVC family, which the engine's translation reads (design 14.4, P7).
    const bool msvc { options.facts && (options.facts->toolchain.family == spec::Family::msvc || options.facts->toolchain.family == spec::Family::clang_cl) };
    std::vector<std::string> baseline;
    if (msvc) baseline = { "/std:c++latest", "/EHsc", "/permissive-", "/MD" };
    else baseline = { "-std=" + options.languageStandard };
    for (std::string_view include : { "include", "src" }) {
        if (const std::string directory { base::join_path(root, include) }; platform::fs::is_directory(directory)) {
            baseline.push_back((msvc ? "/I" : "-I") + directory);
        }
    }
    set.baselineArguments = baseline;

    // A nested project's own sources are not this one's (real-project plan RP3.4).
    const ProjectBoundaries boundaries { root };
    VcpkgPackages vcpkgPackages { root };
    for (const auto& file : platform::fs::list_files(root, SOURCE_EXTENSIONS, SKIPPED_DIRECTORIES)) {
        if (boundaries.crossed(file, root) || vcpkgPackages.contains(file)) continue;
        spec::TranslationUnit unit;
        unit.source = file;
        unit.workDirectory = root;
        unit.arguments.push_back(driver);
        unit.arguments.insert(unit.arguments.end(), baseline.begin(), baseline.end());
        unit.arguments.push_back(msvc ? "/c" : "-c");
        unit.arguments.push_back(file);
        fill_modules(unit, scanner(file));
        set.units.push_back(std::move(unit));
    }
    result.database.sets.push_back(std::move(set));
    return result;
}

} // namespace mcppls::project
