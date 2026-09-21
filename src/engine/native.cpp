module mcppls.engine.native;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.base.version;
import mcppls.lsp.jsonrpc;
import mcppls.lsp.protocol;
import mcppls.normalize.plan;
import mcppls.engine;
import mcppls.engine.native.index;

namespace mcppls::engine::native {

namespace {

std::optional<base::Position> position_of(const Json* params) {
    if (params == nullptr) return std::nullopt;
    const Json* position { lsp::find(*params, "position") };
    if (position == nullptr) return std::nullopt;
    const auto line = lsp::int_at(*position, "line");
    const auto character = lsp::int_at(*position, "character");
    if (!line || !character) return std::nullopt;
    return base::Position { static_cast<int>(*line), static_cast<int>(*character) };
}

class NativeEngine final : public Engine {
private:
    const index::ModuleIndex& index_;
    std::vector<MethodCapability> methods_ {
        { std::string { lsp::method::TEXT_DOCUMENT_DEFINITION }, Role::answer, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_DECLARATION }, Role::answer, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_HOVER }, Role::answer, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_COMPLETION }, Role::answer, 100 },
        { std::string { lsp::method::TEXT_DOCUMENT_DOCUMENT_SYMBOL }, Role::merge, 100 },
        { std::string { lsp::method::WORKSPACE_SYMBOL }, Role::merge, 100 },
    };

    // The module index's answer, or null when the position is not one it answers for.
    Json answer_(const RequestView& request) const {
        const std::string_view method { request.method };
        if (method == lsp::method::TEXT_DOCUMENT_DOCUMENT_SYMBOL) return request.path.empty() ? Json::array() : index_.document_symbols(request.path);
        if (method == lsp::method::WORKSPACE_SYMBOL) {
            return index_.workspace_symbols(request.params != nullptr ? request.params->value("query", std::string {}) : std::string {});
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
    explicit NativeEngine(const index::ModuleIndex& index) : index_ { index } {}

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

std::unique_ptr<Engine> make_engine(const index::ModuleIndex& index) { return std::make_unique<NativeEngine>(index); }

} // namespace mcppls::engine::native
