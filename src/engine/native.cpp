module mcppls.engine.native;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.base.version;
import mcppls.lsp.jsonrpc;
import mcppls.lsp.protocol;
import mcppls.normalize.plan;
import mcppls.project.scan;
import mcppls.orchestrator.tokens;
import mcppls.engine;
import mcppls.engine.native.index;

namespace mcppls::engine::native {

namespace {

namespace tokens = mcppls::orchestrator::tokens;

std::optional<base::Position> position_of(const Json* params) {
    if (params == nullptr) return std::nullopt;
    const Json* position { lsp::find(*params, "position") };
    if (position == nullptr) return std::nullopt;
    const auto line = lsp::int_at(*position, "line");
    const auto character = lsp::int_at(*position, "character");
    if (!line || !character) return std::nullopt;
    return base::Position { static_cast<int>(*line), static_cast<int>(*character) };
}

std::optional<base::Range> range_of(const Json* params) {
    if (params == nullptr) return std::nullopt;
    const Json* range { lsp::find(*params, "range") };
    if (range == nullptr) return std::nullopt;
    const Json* start { lsp::find(*range, "start") };
    const Json* end { lsp::find(*range, "end") };
    if (start == nullptr || end == nullptr) return std::nullopt;
    const auto startLine = lsp::int_at(*start, "line");
    const auto startCharacter = lsp::int_at(*start, "character");
    const auto endLine = lsp::int_at(*end, "line");
    const auto endCharacter = lsp::int_at(*end, "character");
    if (!startLine || !startCharacter || !endLine || !endCharacter) return std::nullopt;
    return base::Range { base::Position { static_cast<int>(*startLine), static_cast<int>(*startCharacter) },
                         base::Position { static_cast<int>(*endLine), static_cast<int>(*endCharacter) } };
}

// Whether a (single-line, per scan_syntax_tokens) token falls inside a requested range.
bool overlaps_range(const base::Range& requested, const base::Range& token) {
    if (token.start.line < requested.start.line || token.start.line > requested.end.line) return false;
    if (token.start.line == requested.start.line && token.end.character <= requested.start.character) return false;
    if (token.start.line == requested.end.line && token.start.character >= requested.end.character) return false;
    return true;
}

// Native module-syntax tokens (design doc 2026-09-25 K/§7): export/module/import keywords and the
// module or partition names that follow -- from a scan of the document text alone, complete or
// not. `range`, given, limits the answer the way `textDocument/semanticTokens/range` requires.
Json semantic_tokens_of(std::string_view text, bool moduleType, const std::optional<base::Range>& range) {
    std::vector<tokens::Token> out;
    for (const auto& syntax : project::scan_syntax_tokens(text)) {
        if (range && !overlaps_range(*range, syntax.range)) continue;
        tokens::Token token { syntax.range.start.line, syntax.range.start.character, syntax.range.end.character - syntax.range.start.character, 0, 0 };
        switch (syntax.kind) {
        case project::SyntaxTokenKind::keyword:
            token.type = tokens::type_index("keyword");
            break;
        case project::SyntaxTokenKind::moduleName:
            token.type = tokens::type_index(moduleType ? "module" : "namespace");
            if (syntax.isDeclaration) token.modifiers |= tokens::modifier_bit("declaration");
            break;
        case project::SyntaxTokenKind::partitionName:
            token.type = tokens::type_index(moduleType ? "module" : "namespace");
            if (syntax.isDeclaration) token.modifiers |= tokens::modifier_bit("declaration");
            if (moduleType) token.modifiers |= tokens::modifier_bit("partition");
            break;
        }
        out.push_back(token);
    }
    return tokens::encode(std::move(out));
}

class NativeEngine final : public Engine {
private:
    const index::ModuleIndex& index_;
    TokenOptions tokenOptions_;
    std::vector<MethodCapability> methods_ {
        { std::string { lsp::method::TEXT_DOCUMENT_DEFINITION }, Role::answer, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_DECLARATION }, Role::answer, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_HOVER }, Role::answer, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_COMPLETION }, Role::answer, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_DOCUMENT_SYMBOL }, Role::merge, 100 },
        { std::string { lsp::method::WORKSPACE_SYMBOL }, Role::merge, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL }, Role::merge, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_RANGE }, Role::merge, 100 },
    };

    // The module index's answer, or null when the position is not one it answers for.
    Json answer_(const RequestView& request) const {
        const std::string_view method { request.method };
        if (method == lsp::method::TEXT_DOCUMENT_DOCUMENT_SYMBOL) return request.path.empty() ? Json::array() : index_.document_symbols(request.path);
        if (method == lsp::method::WORKSPACE_SYMBOL) {
            return index_.workspace_symbols(request.params != nullptr ? request.params->value("query", std::string {}) : std::string {});
        }
        if (method == lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL || method == lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_RANGE) {
            // initializationOptions.semanticTokens.modules == false: mcppls adds no module-syntax
            // tokens of its own (contract T0); the core engine's own tokens, if any, still show.
            if (!tokenOptions_.modules || request.path.empty()) return nullptr;
            const auto range = method == lsp::method::TEXT_DOCUMENT_SEMANTIC_TOKENS_RANGE ? range_of(request.params) : std::nullopt;
            return semantic_tokens_of(request.text, tokenOptions_.moduleType, range);
        }
        if (request.path.empty()) return nullptr;
        const auto position = position_of(request.params);
        if (!position) return nullptr;
        if (method == lsp::method::TEXT_DOCUMENT_DEFINITION || method == lsp::method::TEXT_DOCUMENT_DECLARATION) {
            return index_.definition(request.path, *position);
        }
        if (method == lsp::method::TEXT_DOCUMENT_HOVER) return index_.hover(request.path, *position);
        if (method == lsp::method::TEXT_DOCUMENT_COMPLETION) return index_.completion(request.path, request.text, *position);
        return nullptr;
    }

public:
    explicit NativeEngine(const index::ModuleIndex& index, TokenOptions tokenOptions) : index_ { index }, tokenOptions_ { tokenOptions } {}

    std::string_view id() const override { return ENGINE_ID; }
    std::span<const MethodCapability> methods() const override { return methods_; }
    EngineTraits traits() const override { return EngineTraits { .importNavigation = true, .pushesDiagnostics = true, .tested = true }; }
    EngineStatus status() const override {
        return EngineStatus { .name = std::string { ENGINE_ID }, .version = std::string { base::VERSION }, .role = "modules", .state = "ready", .accepting = true };
    }

    void start(Host& host) override { host.engine_settled(ENGINE_ID, Json::object()); }
    void shut_down() override {}
    void configure_plan(normalize::PlanInput&) const override {}
    void apply(const normalize::EnginePlan*) override {}
    void document(const DocumentEvent&) override {}
    void notify(const Json&) override {}
    void sources_changed() override {}

    bool claims(const RequestView& request) const override {
        if (request.method == lsp::method::TEXT_DOCUMENT_DOCUMENT_SYMBOL) return !request.path.empty();
        if (request.method == lsp::method::WORKSPACE_SYMBOL) return true;
        return !answer_(request).is_null();
    }

    void request(const RequestView& request, const Json&, Reply reply) override {
        reply(Answer { Answer::Kind::result, answer_(request) });
    }

    void cancel(const Json&) override {}
    void client_response(int, const Json&, const Json&) override {}
    void handle_event(const Json&) override {}
    std::optional<Clock::time_point> next_deadline() const override { return std::nullopt; }
    void handle_timers() override {}
};

} // namespace

std::unique_ptr<Engine> make_engine(const index::ModuleIndex& index, TokenOptions tokenOptions) { return std::make_unique<NativeEngine>(index, tokenOptions); }

} // namespace mcppls::engine::native
