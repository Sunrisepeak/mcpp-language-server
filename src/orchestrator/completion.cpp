module mcppls.orchestrator.completion;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.lsp.jsonrpc;

namespace mcppls::orchestrator::completion {

namespace {

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }

std::size_t skip_blanks(std::string_view& text) {
    std::size_t skipped { 0 };
    while (skipped < text.size() && is_blank(text[skipped])) ++skipped;
    text.remove_prefix(skipped);
    return skipped;
}

std::string lowercase(std::string_view text) {
    std::string lowered { text };
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

} // namespace

bool is_import_line_prefix(std::string_view linePrefix) {
    std::string_view rest { linePrefix };
    skip_blanks(rest);
    if (rest.starts_with("export")) {
        std::string_view afterExport { rest.substr(6) };
        // `export` and `import` are two words: `exportimport ` is neither.
        if (skip_blanks(afterExport) > 0) rest = afterExport;
    }
    return rest.size() == 7 && rest.starts_with("import") && is_blank(rest[6]);
}

std::optional<std::string_view> line_prefix(std::string_view text, base::Position position) {
    const auto offset = base::offset_at(text, position);
    if (!offset) return std::nullopt;
    const std::size_t newline { *offset == 0 ? std::string_view::npos : text.rfind('\n', *offset - 1) };
    const std::size_t start { newline == std::string_view::npos ? 0 : newline + 1 };
    return text.substr(start, *offset - start);
}

bool is_space_trigger(const Json& params) {
    const Json* context { lsp::find(params, "context") };
    if (context == nullptr || !context->is_object()) return false;
    return lsp::int_at(*context, "triggerKind").value_or(0) == 2 && context->value("triggerCharacter", std::string {}) == " ";
}

bool vscode_like(const Json& clientParams) {
    const Json* info { lsp::find(clientParams, "clientInfo") };
    if (info == nullptr || !info->is_object()) return false;
    const std::string name { lowercase(info->value("name", std::string {})) };
    if (name.starts_with("visual studio code")) return true;   // and "- Insiders"
    static constexpr std::array<std::string_view, 6> FORKS { "code - oss", "vscodium", "cursor", "windsurf", "trae", "positron" };
    return std::ranges::find(FORKS, name) != FORKS.end();
}

bool space_trigger_wanted(const Json& clientParams) {
    if (const Json* option = lsp::find_path(clientParams, { "initializationOptions", "completion", "triggerOnSpace" }); option != nullptr && option->is_boolean()) {
        return option->get<bool>();
    }
    return vscode_like(clientParams);
}

void add_space_trigger(Json& capabilities) {
    if (!capabilities.is_object()) return;
    Json& provider = capabilities["completionProvider"];
    if (!provider.is_object()) provider = Json::object();
    Json& triggers = provider["triggerCharacters"];
    if (!triggers.is_array()) triggers = Json::array();
    if (std::ranges::find(triggers, Json(" ")) == triggers.end()) triggers.push_back(" ");
}

Json empty_list() { return Json { { "isIncomplete", false }, { "items", Json::array() } }; }

Json merge(const Json& engineResult, const Json& keywordItems) {
    if (!keywordItems.is_array() || keywordItems.empty()) return engineResult;
    Json list = engineResult.is_object() ? engineResult : Json { { "isIncomplete", false }, { "items", engineResult.is_array() ? engineResult : Json::array() } };
    if (!list.contains("items") || !list["items"].is_array()) list["items"] = Json::array();
    if (!list.contains("isIncomplete") || !list["isIncomplete"].is_boolean()) list["isIncomplete"] = false;
    Json& items = list["items"];
    std::set<std::string, std::less<>> labels;
    for (const auto& item : items) {
        if (item.is_object()) labels.emplace(base::trim(item.value("label", std::string {})));
    }
    for (const auto& keyword : keywordItems) {
        if (labels.emplace(base::trim(keyword.value("label", std::string {}))).second) items.push_back(keyword);
    }
    return list;
}

Json keywords_only(const Json& keywordItems) {
    return Json { { "isIncomplete", true }, { "items", keywordItems.is_array() ? keywordItems : Json::array() } };
}

} // namespace mcppls::orchestrator::completion
