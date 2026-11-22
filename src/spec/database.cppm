// S1: the build database with its IDE profile (docs/specs/s1-build-database.md).
// A conforming document is also a P2977R2 build database.
export module mcppls.spec.database;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.spec.metadata;

export namespace mcppls::spec {

inline constexpr std::string_view PROFILE_VERSION { "0.2.0" };

enum class Family { gcc, clang, msvc, clang_cl, other };
enum class Role {
    module_interface,
    module_partition_interface,
    module_partition_implementation,
    module_implementation,
    non_module,
    unknown,
    header_unit,
};

std::string_view to_string(Family family);
std::optional<Family> parse_family(std::string_view text);
std::string_view to_string(Role role);
std::optional<Role> parse_role(std::string_view text);
// A unit another unit can import: an interface, a partition interface or a partition implementation.
bool is_importable(Role role);

struct Stdlib {
    std::string name;             // libstdc++ | libc++ | msvc-stl | other
    std::string version;
    std::string moduleMetadata;   // path of the P3286 manifest
};

struct Toolchain {
    Family family { Family::other };
    std::string version;
    std::string buildId;
    std::string driver;
    std::string target;
    std::string sysroot;
    std::optional<Stdlib> stdlib;
    std::vector<std::string> configFiles;
};

struct Macro {
    std::string name;
    std::optional<std::string> value;
    bool undefine { false };
};

struct IncludeDirectories {
    std::vector<std::string> user;
    std::vector<std::string> quote;
    std::vector<std::string> system;
    std::vector<std::string> after;
};

struct SemanticOptions {
    std::optional<std::string> languageStandard;
    std::optional<std::string> languageExtensions;
    std::vector<Macro> macros;
    IncludeDirectories includeDirectories;
    std::vector<std::string> forcedIncludes;
    std::optional<bool> exceptions;
    std::optional<bool> rtti;
    std::vector<std::pair<std::string, std::vector<std::string>>> rawSemanticArguments;
};

struct TranslationUnit {
    std::string source;
    std::string workDirectory;
    std::vector<std::string> arguments;
    std::vector<std::string> localArguments;
    std::string object;
    bool isPrivate { false };
    std::vector<std::pair<std::string, std::string>> providedModules;   // module name -> BMI path (may be empty)
    std::vector<std::string> requiredModules;
    std::optional<Role> role;
    std::optional<SemanticOptions> options;
    bool optionsDerived { false };   // `options` restate `arguments` (spec::complete_options); the producer stated none
};

// A file the build generates (S1 §7.2): a source a unit is compiled from, a header units include, or a directory of
// them. `origin` is the authored source the file is a transformed copy of, when the producer states it
// (mcpp-community/mcpp#1004, S1 0.4.0); empty otherwise.
struct GeneratedFile {
    std::string path;
    std::string buildPath;
    std::string kind;                         // source | header | directory
    std::string generatorId;
    std::vector<std::string> inputs;
    std::vector<std::string> arguments;
    std::string workDirectory;
    std::string origin;
};

struct Set {
    std::string name;
    std::string familyName;
    std::vector<std::string> visibleSets;
    std::vector<std::string> baselineArguments;
    bool hasIde { false };
    std::string toolchain;
    std::string configuration;
    std::string kind;             // library | executable | test | other
    std::optional<SemanticOptions> options;
    bool optionsDerived { false };   // `options` restate the units' arguments (spec::complete_options); the producer stated none
    std::vector<std::string> moduleMetadata;
    std::vector<GeneratedFile> generated;     // ide.generated
    std::vector<TranslationUnit> units;
};

struct Generator {
    std::string name;
    std::string version;
};

struct Database {
    int version { 1 };
    int revision { 0 };
    bool hasIde { false };
    std::string profileVersion { PROFILE_VERSION };
    std::optional<Generator> generator;
    std::vector<std::pair<std::string, Toolchain>> toolchains;
    std::vector<Set> sets;
};

// Relative paths inside `ide` objects resolve against `databaseDirectory`;
// relative unit paths resolve against the unit's work directory.
base::Result<Database> from_json(const nlohmann::json& document, std::string_view databaseDirectory = {});
nlohmann::ordered_json to_json(const Database& database);
base::Result<Database> load_database(std::string_view path);

// Producer conformance level 1..3 as far as a document can show it; 0 when not even level 1.
int conformance_level(const Database& database);

const Toolchain* find_toolchain(const Database& database, std::string_view id);
std::optional<std::size_t> find_set(const Database& database, std::string_view name);

// mcpp's configuration of the programs a build compiles and runs on the machine doing the build: `build.mcpp`, the
// libraries of `[build-dependencies]` and mcpp's engine interface (mcpp SPEC-005 R3.13, mcpp-community/mcpp#797). Their
// sets see only each other, and no set of another configuration sees them.
inline constexpr std::string_view BUILD_PROGRAM_CONFIGURATION { "build-program" };
// The set of mcpp's engine interface (`import mcpp;`, `import mcpp.core;`): sources mcpp writes once for each of its
// versions, under its home directory, and never edits.
inline constexpr std::string_view ENGINE_INTERFACE_SET { "mcpp:core" };
bool is_build_program_set(const Set& set);
std::string absolute_source(const TranslationUnit& unit);
// The entry of `set`'s generated files a unit's source is, matched by `path` or by `build-path` (the project's build
// output may have replaced the producer's planning directory, project::use_project_build_output); nullptr if none.
const GeneratedFile* generated_source(const Set& set, std::string_view source);
// The authored source a generated source was made from (issue #95): its `origin` when the producer states it, else the
// one input of its step that is a C++ source -- an inference, said to be one by `stated` false. Nothing when the step
// reads no C++ source or several.
struct AuthoredSource {
    std::string path;
    bool stated { false };
};
std::optional<AuthoredSource> authored_source(const GeneratedFile& file);
std::string absolute_path_in_unit(const TranslationUnit& unit, std::string_view path);

// compile_commands.json entries for one set, or for every set (first occurrence of a source wins).
nlohmann::ordered_json to_compile_commands(const Database& database, std::string_view setName = {});

enum class ResolvedFrom { set, visible_set, module_metadata, stdlib, unresolved };

struct Provider {
    std::string source;             // absolute
    std::string set;                // empty for metadata providers
    std::optional<Role> role;
    std::vector<std::string> systemIncludeDirectories;   // metadata providers only
};

struct Resolution {
    ResolvedFrom from { ResolvedFrom::unresolved };
    std::vector<Provider> providers;
    bool ambiguous() const { return providers.size() > 1; }
};

using MetadataReader = std::function<std::vector<ModuleEntry>(std::string_view manifestPath)>;

std::string_view to_string(ResolvedFrom from);
// Section "Module name resolution": set, visible sets, set metadata, toolchain stdlib metadata.
Resolution resolve_module(const Database& database, std::size_t setIndex, std::string_view moduleName, const MetadataReader& reader);
// A reader that caches manifests by path and ignores unreadable ones.
MetadataReader caching_metadata_reader();

} // namespace mcppls::spec
