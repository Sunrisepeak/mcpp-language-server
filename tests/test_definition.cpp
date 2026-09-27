// N-8: picking a module's units by name (design plan §9.1) -- declared_function_at, function_definitions,
// same_function and units_defining, in src/engine/clangd/definition.cppm.
import std;
import mcppls.testing;
import mcppls.base.text;
import mcppls.engine.clangd.definition;

namespace cld = mcppls::engine::clangd;
using mcppls::base::Position;

namespace {

// declared_function_at wants the offset of the name's first character; the declaration tests below
// locate it the same way the definition tests locate a candidate -- by the name's plain text.
std::optional<cld::DeclaredFunction> declared(std::string_view text, std::string_view name) {
    const auto at = text.find(name);
    return at == std::string_view::npos ? std::nullopt : cld::declared_function_at(text, at);
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "a declaration's enclosing scopes and parameters, as written"_test = [] {
        const auto d = declared("export namespace hello {\n    int add(int a, int b);\n}\n", "add");
        expect(fatal(d.has_value()));
        expect(d->name == "add");
        expect(d->scopes == std::vector<std::string> { "hello" });
        expect(d->parameters == std::vector<std::string> { "int", "int" }) << std::format("{}", d->parameters);
    };

    "mcpp's own shape: an implementation partition's declaration, PrepareState& normalized"_test = [] {
        const auto text = "namespace mcpp::build {\n"
                          "std::expected<void, std::string> phase0_manifest_and_workspace(PrepareState& state);\n"
                          "}\n";
        const auto d = declared(text, "phase0_manifest_and_workspace");
        expect(fatal(d.has_value()));
        expect(d->scopes == std::vector<std::string> { "mcpp", "build" }) << std::format("{}", d->scopes);
        expect(d->parameters == std::vector<std::string> { "PrepareState&" }) << std::format("{}", d->parameters);
    };

    "a member function declared inside a nested namespace and class"_test = [] {
        const auto text = "export namespace hello {\n"
                          "class Counter {\n"
                          "public:\n"
                          "    void bump();\n"
                          "};\n"
                          "}\n";
        const auto d = declared(text, "bump");
        expect(fatal(d.has_value()));
        expect(d->scopes == std::vector<std::string> { "hello", "Counter" }) << std::format("{}", d->scopes);
        expect(d->parameters.empty());
    };

    "a template declaration, defaults, a trailing return type, a lambda default -- declaration_kind's own cases still declarations"_test = [] {
        const auto twice = declared("template <class T> T twice(T x);\n", "twice");
        expect(fatal(twice.has_value()));
        expect(twice->parameters == std::vector<std::string> { "T" });
        const auto parse = declared("auto parse(std::string_view text, int base = 10) -> std::optional<Version>;\n", "parse");
        expect(fatal(parse.has_value()));
        expect(parse->parameters == std::vector<std::string> { "std::string_view", "int" }) << std::format("{}", parse->parameters);
    };

    "not a function declaration at all"_test = [] {
        expect(!declared("int x;\n", "x").has_value()) << "a plain variable";
        expect(!declared("void f() {\n}\n", "f").has_value()) << "a definition, not a declaration";
        expect(!declared("struct S;\n", "S").has_value()) << "a type, not a function";
    };

    "mcpp's own shape: the definition, reopening namespace mcpp::build, PrepareState& normalized the same way"_test = [] {
        const auto text = "namespace mcpp::build {\n\n"
                          "std::expected<void, std::string> phase0_manifest_and_workspace(PrepareState& state) {\n"
                          "    return {};\n"
                          "}\n\n"
                          "}\n";
        const auto found = cld::function_definitions(text, "phase0_manifest_and_workspace");
        expect(fatal(found.size() == 1u)) << found.size();
        expect(found[0].qualifiedName == "mcpp::build::phase0_manifest_and_workspace") << found[0].qualifiedName;
        expect(found[0].parameters == std::vector<std::string> { "PrepareState&" }) << std::format("{}", found[0].parameters);

        const auto declText = "namespace mcpp::build {\n"
                              "std::expected<void, std::string> phase0_manifest_and_workspace(PrepareState& state);\n"
                              "}\n";
        const auto d = declared(declText, "phase0_manifest_and_workspace");
        expect(fatal(d.has_value()));
        expect(cld::same_function(*d, found[0]));
    };

    "a free function qualified on its declarator: int hello::add(int a, int b)"_test = [] {
        const auto found = cld::function_definitions("int hello::add(int a, int b) {\n    return a + b;\n}\n", "add");
        expect(fatal(found.size() == 1u));
        expect(found[0].qualifiedName == "hello::add") << found[0].qualifiedName;
        expect(found[0].parameters == std::vector<std::string> { "int", "int" });
        const auto d = declared("export namespace hello {\n    int add(int a, int b);\n}\n", "add");
        expect(fatal(d.has_value()));
        expect(cld::same_function(*d, found[0])) << "a namespace block on one side, the declarator's own qualification on the other";
    };

    "a member function defined out of line matches a declaration inside its class"_test = [] {
        const auto found = cld::function_definitions("void Counter::bump() {\n    ++n_;\n}\n", "bump");
        expect(fatal(found.size() == 1u));
        expect(found[0].qualifiedName == "Counter::bump") << found[0].qualifiedName;
        expect(found[0].parameters.empty());
        const auto d = declared("export namespace hello {\n"
                                "class Counter {\n"
                                "public:\n"
                                "    void bump();\n"
                                "};\n"
                                "}\n",
                                "bump");
        expect(fatal(d.has_value()));
        expect(cld::same_function(*d, found[0])) << "the definition does not spell the enclosing namespace at all";
    };

    "overloads are told apart by their parameter types"_test = [] {
        const auto found = cld::function_definitions(
            "std::string greet(std::string_view name) {\n    return std::string(name);\n}\n"
            "std::string greet(int id) {\n    return std::to_string(id);\n}\n",
            "greet");
        expect(fatal(found.size() == 2u)) << found.size();
        expect(found[0].parameters == std::vector<std::string> { "std::string_view" }) << std::format("{}", found[0].parameters);
        expect(found[1].parameters == std::vector<std::string> { "int" }) << std::format("{}", found[1].parameters);
    };

    "a default argument is dropped, matching a definition that has none"_test = [] {
        const auto d = declared("void f(bool a, bool b = false);\n", "f");
        expect(fatal(d.has_value()));
        expect(d->parameters == std::vector<std::string> { "bool", "bool" }) << std::format("{}", d->parameters);
        const auto found = cld::function_definitions("void f(bool a, bool b) {\n}\n", "f");
        expect(fatal(found.size() == 1u));
        expect(cld::same_function(*d, found[0]));
    };

    "a constructor's initializer list is a definition, and the class name it repeats is not one"_test = [] {
        const auto found = cld::function_definitions("Counter::Counter(int n) : n_ { n } {\n}\n", "Counter");
        expect(fatal(found.size() == 1u)) << found.size();
        expect(found[0].qualifiedName == "Counter::Counter") << found[0].qualifiedName;
        expect(found[0].parameters == std::vector<std::string> { "int" });
    };

    "a destructor's name is written ~Class"_test = [] {
        const auto found = cld::function_definitions("Counter::~Counter() {\n    close();\n}\n", "~Counter");
        expect(fatal(found.size() == 1u));
        expect(found[0].qualifiedName == "Counter::~Counter") << found[0].qualifiedName;
        expect(cld::function_definitions("Counter::~Counter() {\n}\n", "Counter").empty())
            << "a destructor is not a definition of the class's own (constructor) name";
    };

    "comments and strings holding look-alike text are not read as code"_test = [] {
        const auto text = "// int hello::add(int a, int b) { return 0; }\n"
                          "const char* s = \"int hello::add(int a, int b) { return 0; }\";\n"
                          "int hello::add(int a, int b) {\n    return a + b;\n}\n";
        const auto found = cld::function_definitions(text, "add");
        expect(fatal(found.size() == 1u)) << found.size();
        expect(found[0].qualifiedName == "hello::add");
    };

    "a raw string's parentheses do not confuse the parameter scan"_test = [] {
        const auto text = "const char* pattern() {\n"
                          "    return R\"x(add(int, int))x\";\n"
                          "}\n"
                          "int add(int a, int b) {\n    return a + b;\n}\n";
        const auto found = cld::function_definitions(text, "add");
        expect(fatal(found.size() == 1u)) << found.size();
        expect(found[0].parameters == std::vector<std::string> { "int", "int" });
    };

    "a call site is not a definition"_test = [] {
        expect(cld::function_definitions("void caller() {\n    add(1, 2);\n}\n", "add").empty());
        expect(cld::function_definitions("int add(int a, int b);\n", "add").empty()) << "nor is a declaration without a body";
    };

    "a template definition"_test = [] {
        const auto found = cld::function_definitions("template <class T>\nT twice(T x) {\n    return x + x;\n}\n", "twice");
        expect(fatal(found.size() == 1u));
        expect(found[0].parameters == std::vector<std::string> { "T" });
    };

    "a trailing return type"_test = [] {
        const auto found = cld::function_definitions("auto compute() -> int {\n    return 42;\n}\n", "compute");
        expect(fatal(found.size() == 1u));
        expect(found[0].parameters.empty());
    };

    "a const member function defined out of line"_test = [] {
        const auto found = cld::function_definitions("int Widget::value() const {\n    return v_;\n}\n", "value");
        expect(fatal(found.size() == 1u));
        expect(found[0].qualifiedName == "Widget::value");
        expect(found[0].parameters.empty());
    };

    "a definition's name range is where the name is written"_test = [] {
        const std::string_view text { "int add(int a, int b) {\n    return a + b;\n}\n" };
        const auto found = cld::function_definitions(text, "add");
        expect(fatal(found.size() == 1u));
        expect(found[0].nameOffset == text.find("add"));
        expect(found[0].nameRange.start == Position { 0, 4 }) << std::format("{} {}", found[0].nameRange.start.line, found[0].nameRange.start.character);
    };

    "prepare_build: the declaration's defaults on one line, the definition's return type on the line before, neither spelling a default"_test = [] {
        const auto declText =
            "namespace mcpp::build {\n\n"
            "export std::expected<BuildContext, std::string>\n"
            "prepare_build(bool print_fingerprint, bool includeDevDeps = false,\n"
            "              std::vector<mcpp::manifest::Target> extraTargets = {},\n"
            "              BuildOverrides overrides = {});\n\n"
            "}\n";
        const auto d = declared(declText, "prepare_build");
        expect(fatal(d.has_value()));
        expect(d->scopes == std::vector<std::string> { "mcpp", "build" });
        const std::vector<std::string> expectedParams { "bool", "bool", "std::vector<mcpp::manifest::Target>", "BuildOverrides" };
        expect(d->parameters == expectedParams) << std::format("{}", d->parameters);

        const auto defText =
            "namespace mcpp::build {\n\n"
            "std::expected<BuildContext, std::string>\n"
            "prepare_build(bool print_fingerprint,\n"
            "              bool includeDevDeps,\n"
            "              std::vector<mcpp::manifest::Target> extraTargets,\n"
            "              BuildOverrides overrides) {\n"
            "    return {};\n"
            "}\n\n"
            "}\n";
        const auto found = cld::function_definitions(defText, "prepare_build");
        expect(fatal(found.size() == 1u));
        expect(found[0].qualifiedName == "mcpp::build::prepare_build");
        expect(found[0].parameters == expectedParams) << std::format("{}", found[0].parameters);
        expect(cld::same_function(*d, found[0]));
    };

    "same_function refuses a parameter list that differs only because one side names a type alias"_test = [] {
        const auto d = declared("void f(MyAlias a);\n", "f");
        expect(fatal(d.has_value()));
        const auto found = cld::function_definitions("void f(int a) {\n}\n", "f");
        expect(fatal(found.size() == 1u));
        expect(!cld::same_function(*d, found[0])) << "lexically, MyAlias and int are just different spellings";
    };

    "units_defining puts the units that define the name first, in path order, then the rest as given"_test = [] {
        const std::map<std::string, std::string, std::less<>> files {
            { "/p/src/core/a.cpp", "void other() {}\n" },
            { "/p/src/core/target.cpp", "void target() {}\n" },
            { "/p/src/elsewhere/impl.cpp", "namespace n { void target() {} }\n" },
        };
        const auto read = [&](const std::string& path) -> std::optional<std::string> {
            const auto it = files.find(path);
            return it == files.end() ? std::nullopt : std::optional<std::string> { it->second };
        };
        const std::vector<cld::UnitOfModule> units {
            { "/p/src/core/a.cpp", false }, { "/p/src/core/target.cpp", false },
            { "/p/src/elsewhere/impl.cpp", false }, { "/p/src/core/missing.cpp", false },
        };
        const auto chosen = cld::units_defining("target", units, read, 10);
        expect(chosen == std::vector<std::string> { "/p/src/core/target.cpp", "/p/src/elsewhere/impl.cpp", "/p/src/core/a.cpp", "/p/src/core/missing.cpp" })
            << std::format("{}", chosen);
        expect(cld::units_defining("target", units, read, 1) == std::vector<std::string> { "/p/src/core/target.cpp" })
            << "at most `limit`, still a defining unit first";
    };

    return report();
}
