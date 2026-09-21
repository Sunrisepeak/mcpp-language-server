// S1 section 9 from the consumer's side: arguments structured into options, level 2 completed to level 3.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.spec.database;
import mcppls.spec.options;

namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;
namespace s = mcppls::spec;

namespace {

std::string repository_root() {
    std::string directory { fs::current_directory() };
    while (true) {
        if (fs::is_regular_file(base::join_path(directory, "docs/specs/README.md"))) return directory;
        const std::string parent { base::parent_path(directory) };
        if (parent == directory) return fs::current_directory();
        directory = parent;
    }
}

const std::vector<std::string>* raw_of(const s::SemanticOptions& options, std::string_view family) {
    for (const auto& [name, arguments] : options.rawSemanticArguments) {
        if (name == family) return &arguments;
    }
    return nullptr;
}

bool has_macro(const s::SemanticOptions& options, std::string_view name, std::optional<std::string> value, bool undefine = false) {
    return std::ranges::any_of(options.macros, [&](const s::Macro& macro) { return macro.name == name && macro.value == value && macro.undefine == undefine; });
}

s::TranslationUnit unit_of(std::string source, std::vector<std::string> arguments, std::vector<std::string> local = {}) {
    s::TranslationUnit unit;
    unit.source = std::move(source);
    unit.workDirectory = "/p";
    unit.arguments = std::move(arguments);
    unit.localArguments = std::move(local);
    unit.role = s::Role::non_module;
    return unit;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "GCC and Clang arguments structure into options; what only a build needs does not"_test = [] {
        const std::vector<std::string> arguments {
            "-std=gnu++23", "-O2", "-g", "-DNDEBUG", "-D", "LEVEL=3", "-UOLD", "-Iinclude", "-isystem", "/sdk/include", "-iquote", "quote",
            "-idirafter", "after", "-include", "config.h", "-fno-exceptions", "-fno-rtti", "-Wall", "-Werror", "-fmodule-file=a=a.pcm",
            "-fmodule-output=b.pcm", "-MD", "-MF", "x.d", "-o", "x.o", "-c", "--target=x86_64-linux-gnu", "-nostdinc++", "-Xclang",
            "-fno-validate-pch", "-Wp,-DFROM_PREPROCESSOR", "@module.modmap",
        };
        const auto options = s::structure_arguments(arguments, s::Family::gcc);
        expect(options.languageStandard == std::optional<std::string> { "c++23" } && options.languageExtensions == std::optional<std::string> { "gnu" });
        expect(has_macro(options, "NDEBUG", std::nullopt) && has_macro(options, "LEVEL", "3") && has_macro(options, "OLD", std::nullopt, true));
        expect(options.macros.size() == 3u) << "in command-line order, nothing else";
        const auto& directories = options.includeDirectories;
        expect(directories.user == std::vector<std::string> { "include" } && directories.system == std::vector<std::string> { "/sdk/include" });
        expect(directories.quote == std::vector<std::string> { "quote" } && directories.after == std::vector<std::string> { "after" });
        expect(options.forcedIncludes == std::vector<std::string> { "config.h" });
        expect(options.exceptions == std::optional<bool> { false } && options.rtti == std::optional<bool> { false });
        const auto* raw = raw_of(options, "gcc");
        expect(fatal(raw != nullptr));
        expect(*raw == std::vector<std::string> { "--target=x86_64-linux-gnu", "-nostdinc++", "-Xclang", "-fno-validate-pch", "-Wp,-DFROM_PREPROCESSOR" })
            << "raw arguments keep their order and their separate values; optimization, debug information, warnings, outputs,"
               " dependency files and BMI locations are gone (S1-9-1, S1-9-4)";

        const auto plain = s::structure_arguments(std::vector<std::string> { "-std=c++20", "-fexceptions" }, s::Family::clang);
        expect(plain.languageExtensions == std::optional<std::string> { "none" } && plain.exceptions == std::optional<bool> { true });
        expect(plain.rawSemanticArguments.empty());
    };

    "cl.exe and clang-cl arguments structure into options"_test = [] {
        const std::vector<std::string> arguments {
            "/std:c++latest", "/permissive-", "/EHsc", "/GR-", "/DNDEBUG", "/D", "LEVEL=3", "-UOLD", "/Iinclude", "/external:I", "C:/sdk/include",
            "/FIpch.h", "/Zc:__cplusplus", "/MD", "/O2", "/Zi", "/W4", "/Fo:x.obj", "/reference", "std=std.ifc", "/interface", "/TP", "/c",
            "/nologo", "/showIncludes",
        };
        const auto options = s::structure_arguments(arguments, s::Family::msvc);
        expect(options.languageStandard == std::optional<std::string> { "c++latest" } && options.languageExtensions == std::optional<std::string> { "none" });
        expect(options.exceptions == std::optional<bool> { true } && options.rtti == std::optional<bool> { false });
        expect(has_macro(options, "NDEBUG", std::nullopt) && has_macro(options, "LEVEL", "3") && has_macro(options, "OLD", std::nullopt, true));
        expect(options.includeDirectories.user == std::vector<std::string> { "include" });
        expect(options.includeDirectories.system == std::vector<std::string> { "C:/sdk/include" });
        expect(options.forcedIncludes == std::vector<std::string> { "pch.h" });
        const auto* raw = raw_of(options, "msvc");
        expect(fatal(raw != nullptr));
        expect(*raw == std::vector<std::string> { "/Zc:__cplusplus", "/MD" }) << "the runtime library defines macros, so it stays";
        const auto off = s::structure_arguments(std::vector<std::string> { "/EHs-c-", "/GR" }, s::Family::clang_cl);
        expect(off.exceptions == std::optional<bool> { false } && off.rtti == std::optional<bool> { true });
    };

    "a level 2 document completes to level 3, and stated options stay the producer's"_test = [] {
        s::Database database;
        database.hasIde = true;
        s::Toolchain gcc;
        gcc.family = s::Family::gcc;
        gcc.driver = "/opt/gcc/bin/g++";
        database.toolchains.emplace_back("gcc", gcc);

        // With baseline and local arguments, as S1 recommends producers write them.
        s::Set stated;
        stated.name = "pkg";
        stated.hasIde = true;
        stated.toolchain = "gcc";
        stated.baselineArguments = { "-std=c++23", "-O0", "-isystem", "/sdk" };
        stated.units.push_back(unit_of("/p/a.cpp", { "/opt/gcc/bin/g++", "-std=c++23", "-O0", "-isystem", "/sdk", "-c", "/p/a.cpp", "-o", "a.o" }));
        stated.units.push_back(unit_of("src/b.cpp", { "/opt/gcc/bin/g++", "-std=c++23", "-O0", "-isystem", "/sdk", "-DUNIT", "-c", "src/b.cpp" }, { "-DUNIT" }));
        stated.units.push_back(unit_of("/p/c.cppm", { "/opt/gcc/bin/g++", "-std=c++23", "-O0", "-isystem", "/sdk", "-c", "/p/c.cppm" }, { "-Wno-reserved" }));
        database.sets.push_back(stated);

        // Without them: what every unit shares is the set's, the rest each unit's own.
        s::Set shared;
        shared.name = "pkg:test";
        shared.hasIde = true;
        shared.toolchain = "gcc";
        shared.units.push_back(unit_of("/p/t1.cpp", { "/opt/gcc/bin/g++", "-std=c++20", "-DBOTH", "-DONLY", "-c", "/p/t1.cpp", "-o", "t1.o" }));
        shared.units.push_back(unit_of("/p/t2.cpp", { "/opt/gcc/bin/g++", "-std=c++20", "-DBOTH", "-c", "/p/t2.cpp", "-o", "t2.o" }));
        database.sets.push_back(shared);

        expect(s::conformance_level(database) == 2);
        s::complete_options(database);
        const auto& pkg = database.sets[0];
        expect(fatal(pkg.options.has_value()));
        expect(pkg.optionsDerived && pkg.options->languageStandard == std::optional<std::string> { "c++23" });
        expect(pkg.options->includeDirectories.system == std::vector<std::string> { "/sdk" });
        expect(!pkg.units[0].options.has_value()) << "a unit that adds only an output needs no delta";
        expect(fatal(pkg.units[1].options.has_value()));
        expect(pkg.units[1].optionsDerived && has_macro(*pkg.units[1].options, "UNIT", std::nullopt));
        expect(pkg.units[1].options->rawSemanticArguments.empty()) << "a relative source is recognized as the unit's input";
        expect(pkg.units[2].options.has_value()) << "local arguments that change nothing structured still get their (empty) delta";

        const auto& test = database.sets[1];
        expect(fatal(test.options.has_value()));
        expect(test.options->languageStandard == std::optional<std::string> { "c++20" } && has_macro(*test.options, "BOTH", std::nullopt));
        expect(!has_macro(*test.options, "ONLY", std::nullopt));
        expect(fatal(test.units[0].options.has_value()));
        expect(has_macro(*test.units[0].options, "ONLY", std::nullopt) && !test.units[1].options.has_value());
        expect(s::conformance_level(database) == 3);

        // A producer's own options are left as they are, and so are its units.
        s::Database producer = database;
        s::SemanticOptions own;
        own.languageStandard = "c++26";
        producer.sets[1].options = own;
        producer.sets[1].optionsDerived = false;
        producer.sets[1].units[0].options.reset();
        producer.sets[1].units[0].optionsDerived = false;
        s::complete_options(producer);
        expect(producer.sets[1].options->languageStandard == std::optional<std::string> { "c++26" } && !producer.sets[1].optionsDerived);
        expect(!producer.sets[1].units[0].options.has_value());

        // A set whose toolchain family is not known keeps level 2.
        s::Database unknown;
        unknown.hasIde = true;
        s::Toolchain other;
        unknown.toolchains.emplace_back("cc", other);
        s::Set set;
        set.name = "x";
        set.hasIde = true;
        set.toolchain = "cc";
        set.units.push_back(unit_of("/p/x.cpp", { "cc", "-c", "/p/x.cpp" }));
        unknown.sets.push_back(set);
        s::complete_options(unknown);
        expect(!unknown.sets[0].options.has_value() && s::conformance_level(unknown) == 2);
    };

    "local arguments that name the unit's source and output leave both out of its delta"_test = [] {
        // mcpp 2026.9.15.1 writes a standard library unit's command after the set's baseline this way.
        s::Database database;
        database.hasIde = true;
        s::Toolchain clang;
        clang.family = s::Family::clang;
        database.toolchains.emplace_back("llvm", clang);
        s::Set set;
        set.name = "mcpp:std";
        set.hasIde = true;
        set.toolchain = "llvm";
        set.baselineArguments = { "-std=c++23", "-nostdinc++" };
        const std::string source { "/llvm/share/libc++/v1/std.compat.cppm" };
        std::vector<std::string> local { "-fmodule-file=std=/cache/pcm.cache/std.pcm", "--precompile", source, "-o", "pcm.cache/std.compat.pcm" };
        std::vector<std::string> arguments { "/llvm/bin/clang++", "-std=c++23", "-nostdinc++" };
        arguments.insert(arguments.end(), local.begin(), local.end());
        s::TranslationUnit unit { unit_of(source, std::move(arguments), std::move(local)) };
        unit.workDirectory = "/cache";
        set.units.push_back(std::move(unit));
        database.sets.push_back(std::move(set));

        s::complete_options(database);
        const auto& delta = database.sets[0].units[0].options;
        expect(fatal(delta.has_value()));
        expect(delta->rawSemanticArguments.empty()) << "neither the source, the BMI it reads nor the one it writes is an option";
        expect(s::conformance_level(database) == 3);
    };

    "the level 2 example completes to level 3"_test = [] {
        auto database = s::load_database(base::join_path(repository_root(), "docs/specs/examples/s1-level2-clang-two-sets.json"));
        expect(fatal(database.has_value()));
        expect(s::conformance_level(*database) == 2);
        s::complete_options(*database);
        expect(s::conformance_level(*database) == 3);
        for (const auto& set : database->sets) {
            expect(set.optionsDerived && set.options->languageStandard.has_value()) << set.name;
        }
    };

    return report();
}
