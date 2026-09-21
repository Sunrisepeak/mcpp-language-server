// A file's build context (overall design 7.2, S5 4.2): the sets that build it and its role there,
// the toolchain, standard library, language standard and macros, where its semantics come from, and
// what is wrong — so an agent need not guess which compiler and which `std` a file is read with.
export module mcppls.ai.context.build;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;

export namespace mcppls::ai::context {

struct SetMembership {
    std::string name;
    std::string kind;          // library | executable | test | other
    std::string role;          // the unit's role in that set
};

struct BuildIssue {
    std::string code;
    std::string message;
};

struct BuildContext {
    spec::Snapshot snapshot;
    std::string file;
    std::string module;                      // "m" or "m:p"; empty for a unit that is not a module unit
    bool inModel { false };                  // some set of the project model builds the file
    std::vector<SetMembership> sets;
    std::string contextSet;                  // the set the engines read the project as; "default" for all
    std::string projectSource;               // mcpp | cmake | compile-commands | inferred | ...
    std::string semanticSource;              // build-toolchain | semantic-kit
    std::string compiler;                    // "clang 22.1.8"; empty with a kit
    std::string toolchainFamily;
    std::string toolchainVersion;
    std::string target;
    std::string stdlib;                      // "libc++ 22.1.8"
    std::string languageStandard;            // "c++23"
    std::vector<std::string> macros;         // "NAME", "NAME=value", "-NAME" for an undefinition
    std::vector<std::string> includeDirectories;   // the unit's own (user and quote), not the toolchain's
    bool excludedFromEngine { false };       // left out of the core engine's database
    std::vector<BuildIssue> issues;
    std::string engine;                      // "clangd 23.1.0", or "none"
};

query::Outcome<BuildContext> build_context(query::View& view, std::string_view file, query::Clock::time_point deadline);

nlohmann::json to_json(const BuildContext& context);

} // namespace mcppls::ai::context
