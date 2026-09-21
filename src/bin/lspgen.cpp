// mcppls-lspgen: generates mcppls.lsp.protocol from the LSP meta model.
//
//   mcppls-lspgen generate --meta-model <metaModel.json> --out <dir>
//   mcppls-lspgen version
import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.version;
import mcppls.base.path;
import mcppls.platform.fs;

namespace fs = mcppls::platform::fs;
using Json = nlohmann::json;

namespace {

bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower_or_digit(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
char lower(char c) { return is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c; }
char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

// "TypeParameter" -> "type_parameter", "jsonrpcReservedErrorRangeStart" -> "jsonrpc_reserved_error_range_start"
std::string snake(std::string_view name) {
    std::string out;
    for (std::size_t i { 0 }; i < name.size(); ++i) {
        const char c { name[i] };
        if (c == '.' || c == '-' || c == '/' || c == ' ' || c == '$') {
            if (!out.empty() && out.back() != '_') out += '_';
            continue;
        }
        if (is_upper(c) && i > 0) {
            const char previous { name[i - 1] };
            const bool nextLower { i + 1 < name.size() && name[i + 1] >= 'a' && name[i + 1] <= 'z' };
            if (is_lower_or_digit(previous) || (is_upper(previous) && nextLower)) {
                if (!out.empty() && out.back() != '_') out += '_';
            }
        }
        out += lower(c);
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

std::string upper_snake(std::string_view name) {
    std::string out { snake(name) };
    for (auto& c : out) c = upper(c);
    return out;
}

const std::set<std::string, std::less<>>& reserved() {
    static const std::set<std::string, std::less<>> words {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "constexpr", "constinit",
        "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
        "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
        "import", "inline", "int", "long", "module", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
    };
    return words;
}

std::string enumerator(std::string_view name) {
    std::string out { snake(name) };
    if (!out.empty() && out.front() >= '0' && out.front() <= '9') out.insert(0, "n");
    if (reserved().contains(out)) out += '_';
    return out;
}

std::string quote(std::string_view text) {
    std::string out { "\"" };
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

struct Method {
    std::string name;
    bool isRequest { false };
    std::string direction;
    std::string clientCapability;
    std::string serverCapability;
};

std::string direction_enumerator(std::string_view direction) {
    if (direction == "clientToServer") return "client_to_server";
    if (direction == "serverToClient") return "server_to_client";
    return "both";
}

std::string method_constant(std::string_view name) {
    std::string_view trimmed { name };
    if (trimmed.starts_with("$/")) trimmed.remove_prefix(2);
    return upper_snake(trimmed);
}

int generate(const std::string& metaModelPath, const std::string& outDirectory) {
    auto text = fs::read_file(metaModelPath);
    if (!text) {
        std::println(std::cerr, "lspgen: {}", text.error().message);
        return 1;
    }
    Json model = Json::parse(*text, nullptr, false);
    if (model.is_discarded() || !model.is_object()) {
        std::println(std::cerr, "lspgen: {} is not a JSON meta model", metaModelPath);
        return 1;
    }
    const std::string version { model.value(Json::json_pointer { "/metaData/version" }, std::string { "unknown" }) };

    std::vector<Method> methods;
    for (const auto& [key, isRequest] : { std::pair { "requests", true }, std::pair { "notifications", false } }) {
        for (const auto& entry : model.value(key, Json::array())) {
            methods.push_back(Method {
                .name = entry.value("method", std::string {}),
                .isRequest = isRequest,
                .direction = entry.value("messageDirection", std::string { "both" }),
                .clientCapability = entry.value("clientCapability", std::string {}),
                .serverCapability = entry.value("serverCapability", std::string {}),
            });
        }
    }
    std::ranges::sort(methods, {}, &Method::name);

    std::string interface;
    std::string header { std::format("// Generated by mcppls-lspgen from the Language Server Protocol meta model {}.\n"
                                     "// Do not edit: run `mcppls-lspgen generate` instead (tools/lspgen/README.md).\n", version) };
    interface += header;
    interface += "export module mcppls.lsp.protocol;\n\nimport std;\n\n";
    interface += "export namespace mcppls::lsp {\n\n";
    interface += std::format("inline constexpr std::string_view PROTOCOL_VERSION {{ {} }};\n\n", quote(version));
    interface += "enum class MessageDirection { client_to_server, server_to_client, both };\n\n";
    interface += "struct MethodInfo {\n"
                 "    std::string_view name;\n"
                 "    bool isRequest;\n"
                 "    MessageDirection direction;\n"
                 "    std::string_view clientCapability;   // dotted path in ClientCapabilities, or empty\n"
                 "    std::string_view serverCapability;   // dotted path in ServerCapabilities, or empty\n"
                 "};\n\n";
    interface += "// Every request and notification of the protocol, sorted by name.\n";
    interface += "std::span<const MethodInfo> methods();\n";
    interface += "const MethodInfo* find_method(std::string_view name);\n\n";
    interface += "} // namespace mcppls::lsp\n\n";

    interface += "export namespace mcppls::lsp::method {\n\n";
    std::set<std::string> constants;
    for (const auto& method : methods) {
        std::string constant { method_constant(method.name) };
        while (constants.contains(constant)) constant += "_METHOD";
        constants.insert(constant);
        interface += std::format("inline constexpr std::string_view {} {{ {} }};\n", constant, quote(method.name));
    }
    interface += "\n} // namespace mcppls::lsp::method\n\n";

    interface += "export namespace mcppls::lsp {\n";
    for (const auto& enumeration : model.value("enumerations", Json::array())) {
        const std::string name { enumeration.value("name", std::string {}) };
        const std::string base { enumeration.value(Json::json_pointer { "/type/name" }, std::string {}) };
        interface += "\n";
        if (base == "string") {
            interface += std::format("namespace {} {{\n", snake(name));
            for (const auto& value : enumeration.value("values", Json::array())) {
                interface += std::format("inline constexpr std::string_view {} {{ {} }};\n",
                                         upper_snake(value.value("name", std::string {})),
                                         quote(value.value("value", std::string {})));
            }
            interface += std::format("}} // namespace {}\n", snake(name));
        } else {
            const std::string_view underlying { base == "uinteger" ? "std::uint32_t" : "std::int32_t" };
            interface += std::format("enum class {} : {} {{\n", name, underlying);
            for (const auto& value : enumeration.value("values", Json::array())) {
                const Json& number { value.at("value") };
                const std::string literal { number.is_number_integer() ? std::to_string(number.get<std::int64_t>())
                                                                        : number.dump() };
                interface += std::format("    {} = {},\n", enumerator(value.value("name", std::string {})), literal);
            }
            interface += "};\n";
        }
    }
    interface += "\n} // namespace mcppls::lsp\n";

    std::string implementation { header };
    implementation += "module mcppls.lsp.protocol;\n\nimport std;\n\nnamespace mcppls::lsp {\n\nnamespace {\n\n";
    implementation += std::format("constexpr std::array<MethodInfo, {}> METHODS {{ {{\n", methods.size());
    for (const auto& method : methods) {
        implementation += std::format("    MethodInfo {{ {}, {}, MessageDirection::{}, {}, {} }},\n", quote(method.name),
                                      method.isRequest ? "true" : "false", direction_enumerator(method.direction),
                                      quote(method.clientCapability), quote(method.serverCapability));
    }
    implementation += "} };\n\n} // namespace\n\n";
    implementation += "std::span<const MethodInfo> methods() { return METHODS; }\n\n";
    implementation += "const MethodInfo* find_method(std::string_view name) {\n"
                      "    const auto it = std::ranges::lower_bound(METHODS, name, {}, &MethodInfo::name);\n"
                      "    if (it == METHODS.end() || it->name != name) return nullptr;\n"
                      "    return &*it;\n"
                      "}\n\n} // namespace mcppls::lsp\n";

    for (const auto& [file, content] : { std::pair { std::string { "protocol.cppm" }, &interface },
                                         std::pair { std::string { "protocol.cpp" }, &implementation } }) {
        const std::string path { mcppls::base::join_path(outDirectory, file) };
        if (auto written = fs::write_file_atomic(path, *content); !written) {
            std::println(std::cerr, "lspgen: {}", written.error().message);
            return 1;
        }
        std::println("lspgen: wrote {}", path);
    }
    std::println("lspgen: {} methods, {} enumerations from meta model {}", methods.size(),
                 model.value("enumerations", Json::array()).size(), version);
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace mcpplibs;
    int status { 0 };
    // Built statement by statement: builders keep pointers into the App they extend.
    cmdline::App app { "mcppls-lspgen" };
    (void)app.version(std::string { mcppls::base::VERSION });
    (void)app.description("Generate mcppls.lsp.protocol from the LSP meta model");

    cmdline::App versionCommand { "version" };
    (void)versionCommand.description("Print the version");
    (void)versionCommand.action([](const cmdline::ParsedArgs&) { std::println("mcppls-lspgen {}", mcppls::base::VERSION); });
    (void)app.subcommand(std::move(versionCommand));

    cmdline::App generateCommand { "generate" };
    (void)generateCommand.description("Write protocol.cppm and protocol.cpp");
    (void)generateCommand.option("meta-model").takes_value().help("Path to metaModel.json");
    (void)generateCommand.option("out").takes_value().help("Output directory");
    (void)generateCommand.action([&](const cmdline::ParsedArgs& args) {
        status = generate(args.value("meta-model").value_or("tools/lspgen/metaModel-3.18.json"), args.value("out").value_or("src/lsp"));
    });
    (void)app.subcommand(std::move(generateCommand));

    const int parsed { app.run(argc, argv) };
    return parsed != 0 ? parsed : status;
}
