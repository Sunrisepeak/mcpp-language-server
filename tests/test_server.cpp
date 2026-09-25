// Document store, module index and routing, without processes.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.engine.clangd.bmi;
import mcppls.normalize.plan;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native;
import mcppls.engine.native.index;
import mcppls.engine.clangd;
import mcppls.engine.clangd.process;
import mcppls.engine.clangd.definition;
import mcppls.engine.clangd.guard;
import mcppls.engine.clangd.primer;
import mcppls.orchestrator.client;
import mcppls.orchestrator.documents;
import mcppls.orchestrator.incidents;
import mcppls.orchestrator.journal;
import mcppls.orchestrator.routing;
import mcppls.orchestrator.tokens;
import mcppls.orchestrator.workspace;

using Json = nlohmann::json;
using mcppls::base::Position;
namespace idx = mcppls::index;
namespace orch = mcppls::orchestrator;
namespace eng = mcppls::engine;
namespace cld = mcppls::engine::clangd;
namespace tok = mcppls::orchestrator::tokens;

namespace {

// overall design 5.2: a core engine that starts no process, answering every method it is asked with
// a canned result and recording what it was asked.
class FakeCoreEngine : public eng::Engine {
public:
    std::vector<eng::MethodCapability> declared { { std::string { eng::EVERY_METHOD }, eng::Role::answer, 0 },
                                                  { "textDocument/documentSymbol", eng::Role::merge, 0 } };
    std::vector<std::string> asked;
    bool claimsEverything { true };
    Json cannedResult = Json::parse(R"([{"uri": "file:///p/src/core.cpp"}])");

    std::string_view id() const override { return "fake-core"; }
    std::span<const eng::MethodCapability> methods() const override { return declared; }
    eng::EngineTraits traits() const override { return {}; }
    eng::EngineStatus status() const override { return eng::EngineStatus { .name = "fake-core", .role = "core", .state = "ready", .accepting = true }; }
    void start(eng::Host& host) override { host.engine_settled(id(), Json::object()); }
    void shut_down() override {}
    void configure_plan(mcppls::normalize::PlanInput&) const override {}
    void apply(const mcppls::normalize::EnginePlan*) override {}
    void document(const eng::DocumentEvent&) override {}
    void notify(const Json&) override {}
    void sources_changed() override {}
    bool claims(const eng::RequestView&) const override { return claimsEverything; }
    void request(const eng::RequestView& request, const Json&, eng::Reply reply) override {
        asked.emplace_back(request.method);
        reply(eng::Answer { eng::Answer::Kind::result, cannedResult });
    }
    void cancel(const Json&) override {}
    void client_response(int, const Json&, const Json&) override {}
    void handle_event(const Json&) override {}
    std::optional<eng::Clock::time_point> next_deadline() const override { return std::nullopt; }
    void handle_timers() override {}
};

// A clangd that starts and never answers anything, not even initialize.
class SilentProcess : public cld::Process {
public:
    mcppls::base::Result<void> start(const cld::ProcessConfig&, MessageHandler, ClosedHandler, LogHandler) override {
        running_ = true;
        return {};
    }
    mcppls::base::Result<void> send(const Json&) override { return {}; }
    void stop(std::chrono::milliseconds) override { running_ = false; }
    bool running() const override { return running_; }

private:
    bool running_ { false };
};

// A clangd that finishes its handshake and then answers nothing, using the CPU `cpu` says: a fixed
// amount (stuck) or one that grows with the time (busy compiling).
class UnansweringProcess : public cld::Process {
public:
    struct Shared {
        int starts { 0 };
        std::function<std::optional<double>()> cpu;
    };
    explicit UnansweringProcess(std::shared_ptr<Shared> shared) : shared_ { std::move(shared) } {}

    mcppls::base::Result<void> start(const cld::ProcessConfig&, MessageHandler onMessage, ClosedHandler, LogHandler) override {
        onMessage_ = std::move(onMessage);
        running_ = true;
        ++shared_->starts;
        return {};
    }
    mcppls::base::Result<void> send(const Json& message) override {
        if (message.value("method", std::string {}) == "initialize") {
            onMessage_(Json { { "jsonrpc", "2.0" }, { "id", message["id"] }, { "result", Json { { "capabilities", Json::object() } } } });
        }
        return {};
    }
    void stop(std::chrono::milliseconds) override { running_ = false; }
    bool running() const override { return running_; }
    std::function<std::optional<double>()> cpu_reader() const override {
        return [shared = shared_] { return shared->cpu(); };
    }

private:
    std::shared_ptr<Shared> shared_;
    MessageHandler onMessage_;
    bool running_ { false };
};

// A clangd that exits as soon as it is started, before any handshake: optionally after writing
// `line` to its standard error, the way a loader does when clangd cannot run on the machine at all,
// and with the exit reported before or after that line (they come from different threads).
class DyingProcess : public cld::Process {
public:
    struct Shared {
        int starts { 0 };
        std::string line;
        bool exitFirst { false };
    };
    explicit DyingProcess(std::shared_ptr<Shared> shared) : shared_ { std::move(shared) } {}

    mcppls::base::Result<void> start(const cld::ProcessConfig&, MessageHandler, ClosedHandler onClosed, LogHandler onLog) override {
        ++shared_->starts;
        if (shared_->exitFirst) onClosed();
        if (!shared_->line.empty()) onLog(shared_->line);
        if (!shared_->exitFirst) onClosed();
        return {};
    }
    mcppls::base::Result<void> send(const Json&) override { return {}; }
    void stop(std::chrono::milliseconds) override {}
    bool running() const override { return false; }

private:
    std::shared_ptr<Shared> shared_;
};

// The least a Workspace offers an engine: one root, no documents, events recorded.
class RecordingHost : public eng::Host {
public:
    explicit RecordingHost(std::string root) : root_ { std::move(root) }, cache_ { mcppls::base::join_path(root_, "cache") } {}
    std::vector<std::string> events;
    // What the engine's threads sent, to hand back: its process's messages, and its readings of the CPU.
    struct Sunk {
        std::mutex mutex;
        std::vector<Json> events;
    };
    std::shared_ptr<Sunk> sunk { std::make_shared<Sunk>() };

    // What the event loop does: each event an engine's threads produced goes back to that engine.
    void pump(eng::Engine& engine) {
        for (;;) {
            std::vector<Json> taken;
            {
                const std::lock_guard lock { sunk->mutex };
                taken = std::exchange(sunk->events, {});
            }
            if (taken.empty()) return;
            for (const auto& event : taken) engine.handle_event(event);
        }
    }

    const std::string& root_directory() const override { return root_; }
    const std::string& cache_directory() const override { return cache_; }
    const Json& client_initialize_params() const override { return params_; }
    std::function<void(Json)> event_sink(std::string_view) override {
        return [sunk = sunk](Json event) {
            const std::lock_guard lock { sunk->mutex };
            sunk->events.push_back(std::move(event));
        };
    }
    void send_to_client(const Json&) override {}
    std::string client_request_id(std::string_view, int, const Json& id) const override { return id.dump(); }
    void publish_engine_diagnostics(std::string_view, const std::string&, Json, std::optional<std::int64_t>) override {}
    void forget_engine_diagnostics(std::string_view) override {}
    int settled { 0 };   // how often the engine said it settled (the first answers the server's initialize)
    void engine_settled(std::string_view, const Json&) override { ++settled; }
    void status_changed() override {}
    void request_replan() override {}
    std::vector<eng::DocumentView> documents() const override { return {}; }
    bool has_document(std::string_view) const override { return false; }
    std::string engine_uri(std::string_view uri) const override { return std::string { uri }; }
    std::string client_uri(std::string_view uri) const override { return std::string { uri }; }
    void client_view(Json&) const override {}
    std::string path_of_uri(std::string_view uri) const override { return mcppls::base::uri_to_path(uri).value_or(std::string {}); }
    std::vector<std::string> imports_of(std::string_view) const override { return {}; }
    void record_event(std::string_view kind, Json) override { events.emplace_back(kind); }

private:
    std::string root_;
    std::string cache_;
    Json params_ = Json::object();
};

idx::ModuleIndex fixture_index() {
    idx::ModuleIndex index;
    index.update("/p/src/main.cpp", "import std;\nimport hello.greet;\n\nint main() {\n    return 0;\n}\n");
    index.update("/p/src/greet/greet.cppm", "export module hello.greet;\nexport import :detail;\nimport std;\n");
    index.update("/p/src/greet/detail.cppm", "export module hello.greet:detail;\nimport std;\n");
    index.update("/p/src/greet/impl.cpp", "module hello.greet;\nimport missing;\n");
    index.set_external({ idx::ExternalModule { "std", "/kit/share/libc++/v1/std.cppm", "stdlib" },
                         idx::ExternalModule { "std.compat", "/kit/share/libc++/v1/std.compat.cppm", "stdlib" } });
    index.set_profile_label("libc++ 23.1.0 (semantic kit)");
    return index;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "incremental changes use UTF-16 positions"_test = [] {
        orch::DocumentStore store;
        store.open("file:///p/a.cpp", "/p/a.cpp", "cpp", 1, "int a\xF0\x9F\x98\x80 = 1;\nint b = 2;\n");
        const Json changes = Json::parse(R"([
            {"range": {"start": {"line": 0, "character": 7}, "end": {"line": 0, "character": 8}}, "text": "3"},
            {"range": {"start": {"line": 1, "character": 4}, "end": {"line": 1, "character": 5}}, "text": "bee"},
            {"range": {"start": {"line": 2, "character": 0}, "end": {"line": 2, "character": 0}}, "text": "// end\n"}
        ])");
        // "int a😀 = 1;": the emoji takes characters 5 and 6, so character 7 is the space before '='.
        expect(store.change("file:///p/a.cpp", 2, changes));
        const auto* document = store.find("file:///p/a.cpp");
        expect(fatal(document != nullptr));
        expect(document->text == "int a\xF0\x9F\x98\x80" "3= 1;\nint bee = 2;\n// end\n") << document->text;
        expect(document->version == 2);
        expect(store.change("file:///p/a.cpp", 3, Json::parse(R"([{"text": "whole"}])")));
        expect(store.find("file:///p/a.cpp")->text == "whole");
        expect(store.find_by_path("/p/a.cpp") != nullptr);
        store.close("file:///p/a.cpp");
        expect(store.find("file:///p/a.cpp") == nullptr);
        expect(!store.change("file:///p/a.cpp", 4, Json::array()));
    };

    "module names navigate"_test = [] {
        const auto index = fixture_index();
        const Json toInterface = index.definition("/p/src/main.cpp", Position { 1, 9 });
        expect(fatal(toInterface.is_array() && toInterface.size() == 1u));
        expect(toInterface[0]["uri"] == mcppls::base::path_to_uri("/p/src/greet/greet.cppm"));
        expect(toInterface[0]["range"]["start"]["character"] == 14);
        const Json toPartition = index.definition("/p/src/greet/greet.cppm", Position { 1, 15 });
        expect(fatal(toPartition.size() == 1u));
        expect(toPartition[0]["uri"] == mcppls::base::path_to_uri("/p/src/greet/detail.cppm"));
        const Json toStd = index.definition("/p/src/main.cpp", Position { 0, 8 });
        expect(fatal(toStd.size() == 1u));
        expect(toStd[0]["uri"] == mcppls::base::path_to_uri("/kit/share/libc++/v1/std.cppm"));
        const Json implementation = index.definition("/p/src/greet/impl.cpp", Position { 0, 9 });
        expect(fatal(implementation.size() == 1u));
        expect(implementation[0]["uri"] == mcppls::base::path_to_uri("/p/src/greet/greet.cppm"));
        expect(index.definition("/p/src/main.cpp", Position { 3, 5 }).is_null());
    };

    "hover, completion and symbols"_test = [] {
        const auto index = fixture_index();
        const std::string hover { index.hover("/p/src/main.cpp", Position { 1, 12 })["contents"]["value"].get<std::string>() };
        expect(hover.find("module hello.greet") != std::string::npos && hover.find("greet.cppm") != std::string::npos) << hover;
        expect(hover.find("libc++ 23.1.0") != std::string::npos);

        const std::string text { "import std;\nexport import hel" };
        const Json completion = index.completion("/p/src/new.cppm", text, Position { 1, 17 });
        expect(fatal(completion.is_object()));
        expect(completion["items"].size() == 1u && completion["items"][0]["label"] == "hello.greet") << completion.dump();
        expect(completion["items"][0]["textEdit"]["range"]["start"]["character"] == 14);
        const Json partitions = index.completion("/p/src/greet/greet.cppm", "export module hello.greet;\nimport :", Position { 1, 8 });
        expect(partitions["items"].size() == 1u && partitions["items"][0]["label"] == ":detail") << partitions.dump();
        const Json all = index.completion("/p/src/x.cpp", "import ", Position { 0, 7 });
        expect(all["items"].size() == 3u) << all.dump();   // hello.greet, std, std.compat
        expect(index.completion("/p/src/x.cpp", "int important = 1;", Position { 0, 10 }).is_null());
        expect(index.completion("/p/src/x.cpp", "hello::", Position { 0, 7 }).is_null());

        const Json symbols = index.document_symbols("/p/src/greet/detail.cppm");
        expect(symbols.size() == 1u && symbols[0]["name"] == "hello.greet:detail" && symbols[0]["kind"] == 2);
        expect(index.workspace_symbols("GREET").size() == 2u);
        expect(index.workspace_symbols("detail").size() == 1u);
    };

    "module diagnostics"_test = [] {
        auto index = fixture_index();
        const Json impl = index.diagnostics("/p/src/greet/impl.cpp");
        expect(impl.size() == 1u && impl[0]["code"] == "unresolved-module" && impl[0]["source"] == "mcppls") << impl.dump();
        expect(index.diagnostics("/p/src/main.cpp").empty());
        index.update("/p/src/loose.cpp", "import :part;\n");
        expect(index.diagnostics("/p/src/loose.cpp")[0]["code"] == "partition-outside-module");
        index.update("/p/src/copy.cppm", "export module hello.greet;\n");
        expect(index.diagnostics("/p/src/main.cpp")[0]["code"] == "ambiguous-module");
        index.remove("/p/src/copy.cppm");
        expect(index.diagnostics("/p/src/main.cpp").empty());
        index.update("/p/src/greet/noiface.cpp", "module nobody;\n");
        expect(index.diagnostics("/p/src/greet/noiface.cpp")[0]["code"] == "unresolved-module");
    };

    "graph and module info"_test = [] {
        const auto index = fixture_index();
        const Json graph = index.graph();
        bool sawStd { false };
        for (const auto& module : graph["modules"]) {
            if (module["name"] == "std") sawStd = module["external"].get<bool>();
        }
        expect(sawStd);
        expect(graph["imports"].size() >= 5u);
        const Json info = index.module_info("hello.greet");
        expect(info["resolvedFrom"] == "set" && info["providers"].size() == 1u && !info["ambiguous"].get<bool>());
        expect(index.module_info("std")["resolvedFrom"] == "stdlib");
        expect(!index.module_info("nothing").contains("resolvedFrom"));
    };

    "engines declare methods, claim requests, and are selected in order"_test = [] {
        const auto index = fixture_index();
        const auto native = eng::native::make_engine(index);
        FakeCoreEngine core;
        const std::vector<eng::Engine*> engines { native.get(), &core };
        const std::string text { "import std;\nimport hello.greet;\n" };
        const Json onModule = Json::parse(R"({"textDocument": {"uri": "file:///p/src/main.cpp"}, "position": {"line": 1, "character": 10}})");
        const Json elsewhere = Json::parse(R"({"textDocument": {"uri": "file:///p/src/main.cpp"}, "position": {"line": 3, "character": 1}})");
        auto view = [&](std::string_view method, const Json& params) { return eng::RequestView { method, &params, "/p/src/main.cpp", text }; };

        // A module name: mcppls's own engine claims it and answers first; the core engine is the next answerer.
        auto onName = orch::select_engines(engines, view("textDocument/definition", onModule));
        expect(fatal(onName.answerers.size() == 2u));
        expect(onName.mergers.empty());
        expect(onName.answerers[0] == native.get() && onName.answerers[1] == &core);
        expect(orch::select_engines(engines, view("textDocument/hover", onModule)).answerers.front() == native.get());
        // Anywhere else, and methods mcppls's engine does not declare, go to the core engine alone.
        auto other = orch::select_engines(engines, view("textDocument/definition", elsewhere));
        expect(other.answerers.size() == 1u && other.answerers[0] == &core);
        expect(orch::select_engines(engines, view("textDocument/references", onModule)).answerers == std::vector<eng::Engine*> { &core });
        // Outlines merge both. Once one engine merges a method, an engine that answers every method joins the merge.
        expect(orch::select_engines(engines, view("textDocument/documentSymbol", onModule)).mergers.size() == 2u);
        const Json query = Json::parse(R"({"query": "greet"})");
        const eng::RequestView symbols { "workspace/symbol", &query, "", "" };
        expect(orch::select_engines(engines, symbols).mergers == std::vector<eng::Engine*> { native.get(), &core });
        // An engine that does not claim a request (clangd for a unit left out of its database) is not asked.
        core.claimsEverything = false;
        expect(orch::select_engines(engines, view("textDocument/references", onModule)).answerers.empty());
        expect(orch::select_engines(engines, view("textDocument/documentSymbol", onModule)).mergers == std::vector<eng::Engine*> { native.get() });

        // mcppls's engine answers at once, from the index.
        std::optional<eng::Answer> answer;
        native->request(view("textDocument/definition", onModule), Json::object(), [&](eng::Answer a) { answer = std::move(a); });
        expect(fatal(answer.has_value()));
        expect(answer->kind == eng::Answer::Kind::result && answer->value.is_array());
        const std::vector<std::pair<std::string, Json>> merged { { "fake-core", Json::parse(R"([{"name": "main", "kind": 12, "range": {}, "selectionRange": {}}])") },
                                                                 { "mcppls", index.document_symbols("/p/src/greet/detail.cppm") } };
        const Json outline = orch::merge_results("textDocument/documentSymbol", merged);
        expect(outline.size() == 2u && outline[0]["name"] == "hello.greet:detail") << outline.dump();
    };

    // design doc 2026-09-25 K/§7: the native engine's own semantic tokens, and the merge with a
    // (simulated) core engine's already-remapped answer.
    "native engine: semantic tokens, moduleType, modules=false, and the merge"_test = [] {
        const auto index = fixture_index();
        const std::string text { "export module hello.greet:detail;\nimport std;\n" };
        auto view = [&](std::string_view method, const Json& params) { return eng::RequestView { method, &params, "/p/src/greet/detail.cppm", text }; };
        const Json empty = Json::object();

        // moduleType == false (default): module names are `namespace`, no `partition` modifier.
        // Six tokens: export, module, "hello.greet" (declaration), "detail" (declaration
        // partition) on line 0; import, "std" on line 1.
        const auto plain = eng::native::make_engine(index, eng::native::TokenOptions { .modules = true, .moduleType = false });
        std::optional<eng::Answer> answer;
        plain->request(view("textDocument/semanticTokens/full", empty), Json::object(), [&](eng::Answer a) { answer = std::move(a); });
        expect(fatal(answer.has_value() && answer->kind == eng::Answer::Kind::result));
        auto tokens = tok::decode(answer->value);
        expect(fatal(tokens.size() == 6u)) << tokens.size();
        expect(tokens[0].type == tok::type_index("keyword"));                                   // export
        expect(tokens[1].type == tok::type_index("keyword"));                                   // module
        expect(tokens[2].type == tok::type_index("namespace") && tokens[2].modifiers == tok::modifier_bit("declaration"));   // hello.greet
        expect(tokens[3].type == tok::type_index("namespace") && tokens[3].modifiers == tok::modifier_bit("declaration"));   // detail (no partition bit: moduleType is off)
        expect(tokens[4].type == tok::type_index("keyword"));                                   // import
        expect(tokens[5].type == tok::type_index("namespace") && tokens[5].modifiers == 0u);     // std (not a declaration)

        // moduleType == true: the custom `module` type, and the `partition` modifier.
        const auto withModuleType = eng::native::make_engine(index, eng::native::TokenOptions { .modules = true, .moduleType = true });
        answer.reset();
        withModuleType->request(view("textDocument/semanticTokens/full", empty), Json::object(), [&](eng::Answer a) { answer = std::move(a); });
        tokens = tok::decode(answer->value);
        expect(fatal(tokens.size() == 6u));
        expect(tokens[2].type == tok::type_index("module"));
        expect(tokens[3].type == tok::type_index("module") && (tokens[3].modifiers & tok::modifier_bit("partition")) != 0);

        // modules == false: mcppls adds no module-syntax tokens at all, and does not claim the method.
        const auto disabled = eng::native::make_engine(index, eng::native::TokenOptions { .modules = false, .moduleType = false });
        expect(!disabled->claims(view("textDocument/semanticTokens/full", empty)));

        // range: only the second line's tokens (both "import" and "std" are on it).
        const Json rangeParams { { "range", Json { { "start", Json { { "line", 1 }, { "character", 0 } } }, { "end", Json { { "line", 2 }, { "character", 0 } } } } } };
        answer.reset();
        plain->request(view("textDocument/semanticTokens/range", rangeParams), Json::object(), [&](eng::Answer a) { answer = std::move(a); });
        tokens = tok::decode(answer->value);
        expect(fatal(tokens.size() == 2u));
        expect(tokens[0].line == 1 && tokens[1].line == 1);

        // Merge, through routing::merge_results itself: a (simulated) core engine's own answer,
        // already remapped into the server's legend by the workspace (test_tokens.cpp covers that
        // remapping on its own), wins the position it covers; native's answer fills the rest.
        answer.reset();
        plain->request(view("textDocument/semanticTokens/full", empty), Json::object(), [&](eng::Answer a) { answer = std::move(a); });
        expect(fatal(answer.has_value()));
        const Json coreResult = tok::encode(std::vector<tok::Token> { { 0, 0, 6, tok::type_index("keyword"), 0 } });   // covers "export" on line 0
        const std::vector<std::pair<std::string, Json>> mergedInputs { { "fake-core", coreResult }, { "mcppls", answer->value } };
        const Json merged = orch::merge_results("textDocument/semanticTokens/full", mergedInputs);
        const auto mergedTokens = tok::decode(merged);
        expect(std::ranges::any_of(mergedTokens, [](const tok::Token& t) { return t.line == 0 && t.startChar == 0 && t.type == tok::type_index("keyword"); }));
        // Sorted and non-overlapping: no two tokens on the same line share any column.
        for (std::size_t i = 1; i < mergedTokens.size(); ++i) {
            if (mergedTokens[i].line != mergedTokens[i - 1].line) continue;
            expect(mergedTokens[i].startChar >= mergedTokens[i - 1].startChar + mergedTokens[i - 1].length);
        }
    };

    "clangd's module build failures are recognized"_test = [] {
        const auto failure = cld::parse_module_failure(
            R"(E[03:15:19.435] Failed to build module greet; due to Failed to compile C:\Program Files\VS\modules\std.ixx. Use '--log=verbose' to view detailed failure reasons.)");
        expect(fatal(failure.has_value()));
        expect(failure->module == "greet") << failure->module;
        expect(failure->reason == R"(Failed to compile C:\Program Files\VS\modules\std.ixx)") << failure->reason;
        expect(failure->failedSource == R"(C:\Program Files\VS\modules\std.ixx)") << failure->failedSource;
        const auto other = cld::parse_module_failure("E[04:05:38.910] Failed to build module std; due to Don't get the module unit for module std");
        expect(fatal(other.has_value()));
        expect(other->module == "std" && other->failedSource.empty());
        expect(!cld::parse_module_failure("I[04:34:47.305] Built module std to /cache/std.pcm").has_value());
    };

    "a module clangd cannot find is told apart from one that does not compile"_test = [] {
        const auto unresolved = cld::parse_module_failure("E[04:05:38.910] Failed to build module std; due to Don't get the module unit for module std");
        const auto compile = cld::parse_module_failure("E[04:05:39.001] Failed to build module e; due to Failed to compile /p/e.cppm. Use '--log=verbose' to view detailed failure reasons.");
        const auto other = cld::parse_module_failure("E[23:22:24.095] Failed to build module xlings.core.semver; due to Failed to create buffer");
        expect(fatal(unresolved.has_value() && compile.has_value() && other.has_value()));
        expect(cld::failure_kind(*unresolved) == cld::FailureKind::unresolved);
        expect(cld::failure_kind(*compile) == cld::FailureKind::compile);
        expect(cld::failure_kind(*other) == cld::FailureKind::other);
    };

    "restarts in a row are spaced out"_test = [] {
        using namespace std::chrono_literals;
        cld::RestartGate gate;
        const auto t0 = cld::GuardClock::now();
        expect(gate.earliest(t0) == t0) << "the first restart happens at once";
        gate.record(t0);
        expect(gate.earliest(t0 + 1s) == t0 + 10s) << "the next one waits ten seconds";
        gate.record(t0 + 10s);
        expect(gate.earliest(t0 + 11s) == t0 + 30s) << "then twenty";
        expect(gate.earliest(t0 + 31s, cld::RestartCause::plan) == t0 + 31s) << "another cause is spaced out on its own";
        expect(gate.earliest(t0 + 31s, cld::RestartCause::user) == t0 + 31s) << "the person's restart is never held back";
        gate.record(t0 + 31s, cld::RestartCause::user);
        expect(gate.recent(t0 + 32s) == 2u && gate.recent(t0 + 32s, cld::RestartCause::user) == 0u) << "nor counted";
        expect(gate.earliest(t0 + 30min) == t0 + 30min) << "a quiet ten minutes starts over";
    };

    "past the cap a cause backs off, 1, 2, 4, then 8 minutes, and is never refused (fix plan F14)"_test = [] {
        using namespace std::chrono_literals;
        cld::RestartGate gate;
        const auto t0 = cld::GuardClock::now();
        gate.record(t0);
        gate.record(t0 + 10s);
        gate.record(t0 + 30s);
        expect(gate.at_cap(t0 + 31s));
        expect(gate.earliest(t0 + 31s) == t0 + 90s) << "the spacing says 40 s after the last, the first backoff a minute: the later wins";
        expect(!gate.at_cap(t0 + 31s, cld::RestartCause::plan)) << "the plan's budget is its own";
        expect(gate.earliest(t0 + 31s, cld::RestartCause::plan) == t0 + 31s);
        gate.record(t0 + 90s);
        expect(gate.earliest(t0 + 91s) == t0 + 90s + 2min);
        gate.record(t0 + 90s + 2min);
        expect(gate.earliest(t0 + 91s + 2min) == t0 + 90s + 2min + 4min);
        gate.record(t0 + 90s + 6min);
        expect(gate.earliest(t0 + 91s + 6min) == t0 + 90s + 6min + 8min);
        gate.record(t0 + 90s + 14min);
        expect(gate.earliest(t0 + 91s + 14min) <= t0 + 90s + 14min + 8min) << "never longer than eight minutes";
        expect(!gate.at_cap(t0, cld::RestartCause::crash)) << "exits have their own accounting";
        gate.reset();
        expect(!gate.at_cap(t0 + 91s + 14min) && gate.earliest(t0 + 91s + 14min) == t0 + 91s + 14min) << "a model from another source starts over";
    };

    "a file clangd stops answering is set aside; an engine answering nobody is stalled"_test = [] {
        using namespace std::chrono_literals;
        using Verdict = cld::Quarantine::Verdict;
        const auto t0 = cld::GuardClock::now();
        cld::Quarantine quarantine;
        // clangd answered another file after each request was sent: this file is what is stuck.
        expect(quarantine.timed_out("/p/a.cppm", t0, t0 + 10s, t0 + 5s) == Verdict::wait) << "one timeout is not yet a pattern";
        expect(quarantine.timed_out("/p/a.cppm", t0 + 11s, t0 + 21s, t0 + 15s) == Verdict::quarantined);
        expect(quarantine.contains("/p/a.cppm") && quarantine.size() == 1u);
        expect(quarantine.due(t0 + 21s + 1min).empty() && !quarantine.due(t0 + 21s + 2min).empty()) << "the first term is two minutes";
        expect(!quarantine.contains("/p/a.cppm"));
        expect(quarantine.timed_out("/p/a.cppm", t0 + 3min, t0 + 3min + 10s, t0 + 3min + 5s) == Verdict::wait);
        expect(quarantine.timed_out("/p/a.cppm", t0 + 4min, t0 + 4min + 10s, t0 + 4min + 5s) == Verdict::quarantined);
        expect(quarantine.timed_out("/p/a.cppm", t0 + 4min + 1s, t0 + 4min + 11s, t0 + 4min + 6s) == Verdict::wait
               && quarantine.timed_out("/p/a.cppm", t0 + 4min + 2s, t0 + 4min + 12s, t0 + 4min + 7s) == Verdict::wait)
            << "requests sent before the file was set aside time out without setting it aside again";
        expect(quarantine.due(t0 + 4min + 10s + 3min).empty()) << "the second term is longer";
        expect(!quarantine.due(t0 + 4min + 10s + 4min).empty()) << "and not doubled again by the late timeouts";
        expect(quarantine.timed_out("/p/a.cppm", t0 + 9min, t0 + 9min + 10s, t0 + 9min + 5s) == Verdict::wait);
        expect(quarantine.timed_out("/p/a.cppm", t0 + 9min + 11s, t0 + 9min + 21s, t0 + 9min + 15s) == Verdict::quarantined);
        expect(quarantine.release("/p/a.cppm") && !quarantine.contains("/p/a.cppm")) << "a change hands the file back";
        const auto answeredAgain = t0 + 10min;
        quarantine.timed_out("/p/b.cppm", answeredAgain, answeredAgain + 10s, answeredAgain + 5s);
        quarantine.answered("/p/b.cppm");
        expect(quarantine.timed_out("/p/b.cppm", answeredAgain + 20s, answeredAgain + 30s, answeredAgain + 25s) == Verdict::wait) << "an answer starts the count over";

        // clangd answered nothing since the requests were sent, for two files: the engine is stuck.
        cld::Quarantine stalled;
        const auto t1 = t0 + 1h;
        expect(stalled.timed_out("/p/main.cpp", t1, t1 + 10s, t1 - 1s) == Verdict::wait);
        expect(stalled.timed_out("/p/plain.cpp", t1 + 2s, t1 + 12s, t1 - 1s) == Verdict::stalled);
        expect(stalled.first_stalled() == std::optional<std::string> { "/p/main.cpp" }) << "the file asked about first is the likeliest cause";

        // A file rebuilding after a change to it, or to a module it imports, is never set aside for its
        // timeouts, and nothing about it is recorded as set aside -- but clangd answering nobody still counts.
        cld::Quarantine rebuilding;
        const auto t2 = t0 + 2h;
        for (int i { 0 }; i < 4; ++i) {
            expect(rebuilding.timed_out("/p/user.cppm", t2 + i * 11s, t2 + i * 11s + 10s, t2 + i * 11s + 5s, true) == Verdict::wait);
        }
        expect(!rebuilding.contains("/p/user.cppm") && rebuilding.size() == 0u) << "not set aside, not even in the books";
        expect(rebuilding.timed_out("/p/main.cpp", t2 + 1min, t2 + 1min + 10s, t2 - 1s, true) == Verdict::wait);
        expect(rebuilding.timed_out("/p/plain.cpp", t2 + 1min + 2s, t2 + 1min + 12s, t2 - 1s, true) == Verdict::stalled)
            << "answering nobody is clangd, whatever changed";
    };

    "clangd's state for a file says whether it is working on it"_test = [] {
        expect(cld::engine_working("parsing main file") && cld::engine_working("parsing includes") && cld::engine_working("running Hover"));
        expect(cld::engine_working("parsing includes, file is queued")) << "building its preamble while the file waits for a worker";
        expect(!cld::engine_working("idle") && !cld::engine_working("file is queued") && !cld::engine_working("preamble (queued)"));
        expect(!cld::engine_working("preamble (queued), file is queued") && !cld::engine_working(""));
    };

    "a location is a declaration only when nothing defines it there"_test = [] {
        using Kind = cld::DeclarationKind;
        const auto kind = [](std::string_view text, std::string_view name) {
            return cld::declaration_kind(text, text.find(name) + name.size());
        };
        expect(kind("export void emit(const Diagnostic& d);\n", "emit") == Kind::declaration);
        expect(kind("void emit(const Diagnostic& d) {\n}\n", "emit") == Kind::definition);
        expect(kind("export int answer() { return 42; }", "answer") == Kind::definition);
        expect(kind("struct Segment {\n    static Segment number(unsigned long long v);\n};", "number") == Kind::declaration) << "a member declared in its class";
        expect(kind("auto parse(std::string_view text, int base = 10) -> std::optional<Version>;", "parse") == Kind::declaration) << "default arguments and a trailing return";
        expect(kind("void run(std::function<void()> f = [] { return; });", "run") == Kind::declaration) << "a lambda in a default argument";
        expect(kind("template <class T> requires std::integral<T> T twice(T x) noexcept(true);", "twice") == Kind::declaration);
        expect(kind("Widget::Widget(int x) : value { x } {}", "Widget::Widget") == Kind::definition) << "a constructor's initializers";
        expect(kind("struct Model : Base { int x; };", "Model") == Kind::definition && kind("struct Model;", "Model") == Kind::declaration);
        expect(kind("Widget(const Widget&) = default;", "Widget") == Kind::definition && kind("extern int counter;", "counter") == Kind::declaration);
        expect(kind("int limit = 3;", "limit") == Kind::definition && kind("std::vector<std::string> names;", "names") == Kind::declaration);
        expect(kind("void f(/* ; */ int x) // {\n;", "f") == Kind::declaration) << "comments are not code";
        expect(kind("void f(const char* s = \";{\");", "f") == Kind::declaration) << "nor are strings";
        expect(kind("enum class Color { red, green };", "red") == Kind::unknown);
    };

    "the units searched for a definition come in the order they likely hold it"_test = [] {
        const std::vector<cld::UnitOfModule> units { { "/p/src/core/detail.cppm", true }, { "/p/src/core/other.cpp", false },
                                                     { "/p/src/core/diag.cpp", false }, { "/p/src/elsewhere/diag_impl.cpp", false } };
        const auto chosen = cld::units_to_search("/p/src/core/diag.cppm", units, 3);
        expect(chosen == std::vector<std::string> { "/p/src/core/diag.cpp", "/p/src/core/other.cpp", "/p/src/elsewhere/diag_impl.cpp" }) << std::format("{}", chosen);
        expect(cld::units_to_search("/p/src/core/diag.cppm", units, 10).back() == "/p/src/core/detail.cppm") << "partitions last";
    };

    "clangd's log is forwarded without flooding"_test = [] {
        using namespace std::chrono_literals;
        cld::LineLimiter limiter { 3, 10s };
        const auto t0 = cld::GuardClock::now();
        int forwarded { 0 };
        for (int i { 0 }; i < 1000; ++i) forwarded += limiter.admit(t0 + std::chrono::milliseconds { i }).forward ? 1 : 0;
        expect(forwarded == 3) << forwarded;
        const auto next = limiter.admit(t0 + 11s);
        expect(next.forward && next.suppressedBefore == 997u) << next.suppressedBefore;
        expect(limiter.admit(t0 + 12s).suppressedBefore == 0u);
    };

    "the journal keeps the latest events and counts them all"_test = [] {
        orch::Journal journal;
        for (std::size_t i { 0 }; i < orch::Journal::CAPACITY + 20; ++i) journal.add("request-timeout", Json { { "n", i } });
        journal.add("engine-restart", Json { { "reason", "a module's unit left the engine database" } });
        const Json recent = journal.recent(3);
        expect(fatal(recent.size() == 3u));
        expect(recent[2]["kind"] == "engine-restart" && recent[2]["detail"]["reason"] == "a module's unit left the engine database");
        expect(recent[1]["detail"]["n"] == orch::Journal::CAPACITY + 19) << "oldest first, newest last";
        expect(recent[0]["at"].get<std::string>().ends_with("Z"));
        expect(journal.recent(10000).size() == orch::Journal::CAPACITY) << "only the latest are kept";
        expect(journal.total("request-timeout") == orch::Journal::CAPACITY + 20) << "every one is counted";
        expect(journal.totals()["engine-restart"] == 1 && journal.total("engine-exit") == 0u);
    };

    "clangd takes a quarter of the cores, and module preparation half of that"_test = [] {
        expect(cld::engine_workers(32, false) == 4u) << "16 cores";
        expect(cld::engine_workers(128, false) == 16u);
        expect(cld::engine_workers(10, true) == 2u) << "macOS counts cores as threads";
        expect(cld::engine_workers(4, false) == 2u && cld::engine_workers(0, false) == 2u) << "never less than two";
        expect(cld::preparation_limit(32, false, 0) == 2u) << "half of clangd's workers";
        expect(cld::preparation_limit(32, false, 2) == 1u) << "a quarter while a person waits on something else";
        expect(cld::preparation_limit(128, false, 0) == 8u && cld::preparation_limit(128, false, 1) == 4u);
        expect(cld::preparation_limit(4, false, 0) == 1u && cld::preparation_limit(0, false, 5) == 1u) << "never less than one";
    };

    // The throttle above is right only when the file being waited for can progress without
    // preparation. A modules TU usually cannot: it is blocked on exactly these BMIs, so throttling
    // starves the work that would answer it and the freed workers idle. Measured before this
    // distinction existed: one busy core of 32 through a cold start.
    // Counting `*.pcm` reports twice as many modules as exist, because the canonical copy sits
    // beside clangd's stamped one. The first cut of this parse walked backwards testing
    // "everything after this dash is digits", which the second dash always fails — it returned
    // every name unchanged and `devtools cache` said 222 modules where 111 exist.
    "a BMI file name gives back the module it belongs to"_test = [] {
        expect(cld::module_of_bmi("xlings.core.utf8-20260917-014433-869144.pcm") == "xlings.core.utf8");
        expect(cld::module_of_bmi("xlings.core.utf8.pcm") == "xlings.core.utf8") << "the canonical copy";
        expect(cld::module_of_bmi("std-20260917-014433-869144.pcm") == "std");
        expect(cld::module_of_bmi("std.pcm") == "std");
        // A module name may contain dashes and digits of its own; only the full stamp is a stamp.
        expect(cld::module_of_bmi("my-module-20260917-014433-1.pcm") == "my-module");
        expect(cld::module_of_bmi("my-module.pcm") == "my-module");
        expect(cld::module_of_bmi("v2-20260917-014433-7.pcm") == "v2");
        // Not stamps: wrong field widths, a non-numeric serial, a missing field.
        expect(cld::module_of_bmi("a-2026091-014433-7.pcm") == "a-2026091-014433-7") << "date is 7 digits";
        expect(cld::module_of_bmi("a-20260917-01443-7.pcm") == "a-20260917-01443-7") << "time is 5 digits";
        expect(cld::module_of_bmi("a-20260917-014433-x.pcm") == "a-20260917-014433-x") << "serial not numeric";
        expect(cld::module_of_bmi("a-20260917-014433.pcm") == "a-20260917-014433") << "no serial field";
        expect(cld::module_of_bmi("plain") == "plain");
        expect(cld::module_of_bmi("trailing-.pcm") == "trailing-");
    };

    "preparation is not throttled by a file that is waiting for preparation"_test = [] {
        expect(cld::preparation_limit(32, false, 2, true) == 2u) << "the waiting file needs these very modules";
        expect(cld::preparation_limit(32, false, 2, false) == 1u) << "it waits on something else: throttle";
        expect(cld::preparation_limit(32, false, 0, true) == 2u) << "nobody waiting is the same as before";
        expect(cld::preparation_limit(128, false, 4, true) == 8u) << "scales with the machine";
        expect(cld::preparation_limit(128, false, 4, false) == 4u);
        // The default keeps every existing caller on the pre-2026-09-17 behaviour.
        expect(cld::preparation_limit(32, false, 2) == cld::preparation_limit(32, false, 2, false));
        cld::ProcessConfig config;
        config.workers = 4;
        const auto defaults = cld::clangd_arguments(config);
        expect(std::ranges::find(defaults, std::string { "-j=4" }) != defaults.end());
        config.extraArguments = { "-j=16" };
        const auto arguments = cld::clangd_arguments(config);
        expect(std::ranges::count_if(arguments, [](const std::string& argument) { return argument.starts_with("-j"); }) == 1) << "an explicit -j wins";
    };

    "clangd runs at info, not error, so an incident says what it was doing (fix plan F17.1)"_test = [] {
        cld::ProcessConfig config;
        const auto arguments = cld::clangd_arguments(config);
        expect(std::ranges::find(arguments, std::string { "--log=info" }) != arguments.end());
        config.verboseLog = true;
        const auto verbose = cld::clangd_arguments(config);
        expect(std::ranges::find(verbose, std::string { "--log=verbose" }) != verbose.end());
    };

    "clangd's crash context names the file it crashed on (fix plan F3)"_test = [] {
        // As the Windows CI of issue #23 printed it (GalTranslPP, clangd 23.1.0).
        cld::LogReader reader;
        expect(reader.read("I[18:14:51.802] Built prerequisite modules for file D:\\a\\G\\N.Core.cpp in 0.01 seconds").important == false);
        expect(reader.read("PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/ and include the crash backtrace.").important);
        auto read = reader.read("Signalled during AST worker action: Build AST");
        expect(read.important && !read.crash) << "not before its file is known";
        read = reader.read("  Filename: D:/a/G/G/NormalJsonTranslator.Core.cpp");
        expect(fatal(read.crash.has_value()));
        expect(read.crash->action == "Build AST" && read.crash->file == "D:/a/G/G/NormalJsonTranslator.Core.cpp" && read.crash->exception.empty());
        expect(reader.read("  Directory: D:/a/G").important && !reader.read("  Command Line: clang++ --driver-mode=g++ -- x.cpp").crash);
        expect(!reader.read("  Version: 1").crash);
        read = reader.read("Exception Code: 0x80000003");
        expect(fatal(read.crash.has_value()));
        expect(read.crash->exception == "0x80000003" && read.crash->file == "D:/a/G/G/NormalJsonTranslator.Core.cpp");
        // A preamble build, POSIX: no exception code, the context is complete with its file.
        cld::LogReader posix;
        expect(!posix.read("Signalled while building preamble").crash);
        read = posix.read("  Filename: /p/src/main.cpp");
        expect(fatal(read.crash.has_value()));
        expect(read.crash->action == "building preamble" && read.crash->file == "/p/src/main.cpp");
        expect(!posix.read("  Filename: /p/other.cpp").crash) << "one file per context";
    };

    "a failed module scan is read with its reason, whether the driver's or the source's (fix plan F6)"_test = [] {
        cld::LogReader reader;
        // #23: a command without -c, with -flto, for windows-msvc.
        auto read = reader.read("E[17:58:04.111] Scanning modules dependencies for D:\\a\\G\\GPPDefines.ixx failed: clang++: error: LTO requires -fuse-ld=lld");
        expect(read.important && !read.scanFailure) << "it lasts until its closing line";
        read = reader.read("E[17:58:04.111] The command line the scanning tool use is: clang++ --driver-mode=g++ -flto -- GPPDefines.ixx");
        expect(fatal(read.scanFailure.has_value()));
        expect(read.scanFailure->file == "D:\\a\\G\\GPPDefines.ixx" && read.scanFailure->reason == "error: LTO requires -fuse-ld=lld" && read.scanFailure->driver);
        // A header not found: the reason is on a continuation line, after an "In file included from".
        expect(!reader.read("E[17:58:04.112] Scanning modules dependencies for /p/a.cpp failed: In file included from /p/a.cpp:1:").scanFailure);
        expect(reader.read("In file included from /p/a.h:4:").important);
        expect(!reader.read("/p/b.h:4:10: fatal error: 'ElaScrollPage.h' file not found").scanFailure);
        expect(!reader.read("").scanFailure);
        read = reader.read("E[17:58:04.114] The command line the scanning tool use is: clang++ -- /p/a.cpp");
        expect(fatal(read.scanFailure.has_value()));
        expect(read.scanFailure->reason == "fatal error: 'ElaScrollPage.h' file not found" && !read.scanFailure->driver);
        // A scan that never got its closing line ends with the next line that has a severity, or the stream.
        expect(!reader.read("E[1] Scanning modules dependencies for /p/c.cppm failed: /p/c.cppm:1:26: error: expected identifier after '.' in module name").scanFailure);
        read = reader.read("I[2] ASTWorker building file /p/c.cppm");
        expect(fatal(read.scanFailure.has_value()));
        expect(read.scanFailure->file == "/p/c.cppm" && read.scanFailure->reason == "error: expected identifier after '.' in module name" && !read.scanFailure->driver);
        expect(!reader.read("E[3] Scanning modules dependencies for /p/d.cpp failed: x").scanFailure);
        expect(reader.finish().has_value() && !reader.finish().has_value());
        expect(!reader.read("I[4] Failed to build module greet").important) << "an everyday line is not important";
    };

    "the log ring keeps the latest lines within its bounds (fix plan F17.1)"_test = [] {
        cld::LogRing ring { 3, 1000 };
        for (int i { 0 }; i < 5; ++i) ring.add(std::format("line {}", i));
        expect(ring.size() == 3u);
        const std::string text { ring.text() };
        expect(text.starts_with("[2 earlier lines are not kept]\n") && text.ends_with("line 4\n") && !text.contains("line 1\n")) << text;
        cld::LogRing small { 100, 12 };
        small.add("0123456789");
        small.add("abc");
        expect(small.size() == 1u && small.text().ends_with("abc\n")) << "bytes bound it too";
    };

    "incidents are named by their time and kind, and only the newest stay (fix plan F17.2, F17.7)"_test = [] {
        namespace incidents = mcppls::orchestrator::incidents;
        namespace fs = mcppls::platform::fs;
        using namespace std::chrono_literals;
        const auto at = std::chrono::sys_days { std::chrono::year { 2026 } / 9 / 26 } + 7h + 19min + 46s + 638ms;
        const std::string name { incidents::directory_name("engine crash/x", at) };
        expect(name == "20260926T071946.638Z-engine-crash-x") << name;
        expect(incidents::time_of(name) == std::optional<std::chrono::system_clock::time_point> { std::chrono::floor<std::chrono::seconds>(at) });
        expect(!incidents::time_of("notes").has_value() && !incidents::time_of("20261399T071946.638Z-x").has_value());
        const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-incidents-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        const std::string directory { mcppls::base::join_path(root, "incidents") };
        for (int i { 0 }; i < 25; ++i) (void)fs::create_directories(mcppls::base::join_path(directory, incidents::directory_name("spin", at + std::chrono::minutes { i })));
        (void)fs::create_directories(mcppls::base::join_path(directory, "not-an-incident"));
        incidents::prune(directory, at + 30min);
        expect(fs::list_directory(directory).size() == incidents::KEEP + 1) << "the newest twenty, and what is not an incident";
        incidents::prune(directory, at + 24h * 7 + 10min);
        expect(fs::list_directory(directory).size() == 1u + 14u) << "none older than a week";
        auto written = incidents::write(root, "file-set-aside", Json { { "kind", "file-set-aside" } },
                                        { mcppls::engine::IncidentFile { "clangd.log", "E[1] x\n" }, mcppls::engine::IncidentFile { "../escape.txt", "no" } }, std::nullopt);
        expect(fatal(written.has_value()));
        const auto incident = Json::parse(fs::read_file(mcppls::base::join_path(*written, "incident.json")).value_or("{}"));
        expect(incident["attached"] == Json::array({ "clangd.log", "escape.txt" }) && fs::exists(mcppls::base::join_path(*written, "clangd.log")));
        expect(!fs::exists(mcppls::base::join_path(root, "escape.txt"))) << "a file never lands outside its incident";
        fs::remove_all(root);
    };

    "merging"_test = [] {
        const Json engineSymbols = Json::parse(R"([{"name": "hello", "kind": 3, "range": {}, "selectionRange": {}}])");
        const Json moduleSymbols = Json::parse(R"([{"name": "hello.greet", "kind": 2, "range": {}, "selectionRange": {}}])");
        const Json merged = orch::merge_document_symbols(engineSymbols, moduleSymbols);
        expect(merged.size() == 2u && merged[0]["name"] == "hello.greet");
        const Json flat = Json::parse(R"([{"name": "hello", "kind": 3, "location": {}}])");
        expect(orch::merge_document_symbols(flat, moduleSymbols).size() == 1u);
        expect(orch::merge_workspace_symbols(nullptr, moduleSymbols).size() == 1u);

        const Json range = Json::parse(R"({"start": {"line": 1, "character": 7}, "end": {"line": 1, "character": 18}})");
        const Json moduleDiagnostics = Json::array({ Json { { "range", range }, { "message", "module 'x' not found" }, { "source", "mcppls" } } });
        const Json engineDiagnostics = Json::array({ Json { { "range", range }, { "message", "module 'x' not found" }, { "source", "clang" } },
                                                     Json { { "range", Json::parse(R"({"start": {"line": 4, "character": 0}, "end": {"line": 4, "character": 1}})") }, { "message", "other" }, { "source", "clang" } } });
        const Json diagnostics = orch::merge_diagnostics(engineDiagnostics, moduleDiagnostics, "gcc 16.1.0");
        expect(diagnostics.size() == 2u) << diagnostics.dump();
        expect(diagnostics[1]["source"] == "clang \xC2\xB7 gcc 16.1.0") << diagnostics.dump();

        const Json capabilities = orch::merge_capabilities(Json::parse(R"({"hoverProvider": true, "textDocumentSync": {"change": 2}})"));
        expect(capabilities["experimental"]["cxxModules"]["version"] == 1);
        expect(capabilities["definitionProvider"] == true && capabilities["textDocumentSync"]["change"] == 2);
        const Json client = Json::parse(R"({"experimental": {"cxxModules": {"version": 1, "status": true}}})");
        expect(orch::client_supports(client, "status") && !orch::client_supports(client, "graph"));
    };

    "modules are prepared as soon as their imports are, within the limit"_test = [] {
        using State = cld::Primer::State;
        cld::Primer primer;
        primer.set_limit(2);
        primer.set_modules({
            { "std", {}, "/prime/std.cpp" },
            { "base", { "std" }, "/prime/base.cpp" },
            { "util", { "std" }, "/prime/util.cpp" },
            { "app:part", { "base" }, "" },
            { "app", { "app:part", "util" }, "/prime/app.cpp" },
            { "tool", { "std" }, "/prime/tool.cpp" },
        });
        const std::vector<std::string> wanted { "app" };
        expect(primer.want(wanted) == 5u) << "everything app reaches, and nothing else";
        expect(primer.state("tool") == State::unwanted);
        expect(primer.busy());
        auto names = [](const std::vector<const cld::PrimeModule*>& modules) {
            std::vector<std::string> result;
            for (const auto* module : modules) result.push_back(module->name);
            return result;
        };
        expect(names(primer.start_ready()) == std::vector<std::string> { "std" }) << "only std has no imports";
        expect(primer.start_ready().empty()) << "a started module is not started twice";
        primer.finish("std");
        expect(names(primer.start_ready()) == std::vector<std::string> { "base", "util" });
        expect(primer.running() == 2u);
        primer.finish("base");
        // app:part has no unit of its own: it completes with base, and app still waits for util.
        expect(primer.start_ready().empty());
        expect(primer.state("app:part") == State::done);
        primer.finish("util");
        expect(names(primer.start_ready()) == std::vector<std::string> { "app" });
        expect(primer.progress() == std::pair<std::size_t, std::size_t> { 4u, 5u });
        primer.finish("app");
        expect(!primer.busy());
        primer.finish("app");
        expect(primer.running() == 0u) << "a module finished twice is counted once";

        const std::vector<cld::PrimeModule> smaller { { "std", {}, "/prime/std.cpp" }, { "base", { "std" }, "/prime/base.cpp" } };
        expect(!primer.same_modules(smaller));
        const std::vector<cld::PrimeModule> otherImports {
            { "std", {}, "/prime/std.cpp" },       { "base", { "std" }, "/prime/base.cpp" },          { "util", { "std", "base" }, "/prime/util.cpp" },
            { "app:part", { "base" }, "" },         { "app", { "app:part", "util" }, "/prime/app.cpp" }, { "tool", { "std" }, "/prime/tool.cpp" },
        };
        expect(!primer.same_modules(otherImports)) << "util imports base now";
        const std::vector<cld::PrimeModule> reordered {
            { "tool", { "std" }, "/prime/tool.cpp" }, { "app", { "app:part", "util" }, "/prime/app.cpp" }, { "app:part", { "base" }, "" },
            { "util", { "std" }, "/prime/util.cpp" }, { "base", { "std" }, "/prime/base.cpp" },          { "std", {}, "/prime/std.cpp" },
        };
        expect(primer.same_modules(reordered)) << "the order modules are listed in does not matter";

        // A new graph keeps what is done; reset forgets it, as after an engine restart.
        primer.set_modules(smaller);
        expect(primer.state("std") == State::done && primer.state("base") == State::done);
        expect(primer.find("app") == nullptr && primer.find("base") != nullptr);
        primer.reset();
        expect(primer.state("std") == State::unwanted && !primer.busy());
    };

    "a request about a file still being prepared waits while preparation progresses"_test = [] {
        using namespace std::chrono_literals;
        const auto now = eng::Clock::now();
        cld::PendingRequest request;
        request.purpose = cld::Purpose::client;
        request.deadline = now;
        request.limit = now + 50s;
        expect(cld::keep_waiting(request, true, now - 2s, now)) << "a module finished two seconds ago";
        expect(!cld::keep_waiting(request, false, now - 2s, now)) << "the file's modules are ready: answer";
        expect(!cld::keep_waiting(request, true, std::nullopt, now)) << "nothing has finished yet: no sign of progress";
        expect(!cld::keep_waiting(request, true, now - 11s, now)) << "no module finished for longer than a request's own wait";
        request.limit = now;
        expect(!cld::keep_waiting(request, true, now - 2s, now)) << "the request's limit is reached";
        request.limit = now + 50s;
        request.purpose = cld::Purpose::engine_initialize;
        expect(!cld::keep_waiting(request, true, now - 2s, now)) << "only a client's request waits";
    };

    "a request is answered by its limit whatever holds clangd up"_test = [] {
        using namespace std::chrono_literals;
        const auto now = eng::Clock::now();
        expect(cld::wait_limit("textDocument/hover", 60s, now) == now + cld::INTERACTIVE_LIMIT) << "a person's request: the interactive limit";
        expect(cld::wait_limit("textDocument/hover", 5s, now) == now + 5s) << "a shorter configured timeout still applies";
        expect(cld::wait_limit("textDocument/references", 60s, now) == now + 60s) << "the rest: the configured timeout";

        // real-project plan RP1.1: a clangd that never finishes its handshake leaves the request
        // waiting in the engine's own queue; at its limit it is answered unavailable, so the next
        // engine answers it, rather than never.
        namespace fs = mcppls::platform::fs;
        const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-limit-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        (void)fs::create_directories(root);
        const std::string executable { mcppls::base::join_path(root, "clangd") };
        (void)fs::write_file(executable, "pretend-clangd");
        cld::Options options;
        options.executable = executable;
        options.version = "23.1.0";
        options.requestTimeout = 1s;
        options.processFactory = [] { return std::make_unique<SilentProcess>(); };
        RecordingHost host { root };
        auto engine = cld::make_engine(std::move(options));
        engine->start(host);
        const std::string file { mcppls::base::join_path(root, "a.cpp") };
        const Json params { { "textDocument", Json { { "uri", mcppls::base::path_to_uri(file) } } }, { "position", Json { { "line", 0 }, { "character", 0 } } } };
        const Json message { { "jsonrpc", "2.0" }, { "id", 7 }, { "method", "textDocument/hover" }, { "params", params } };
        std::optional<eng::Answer> answer;
        const auto asked = eng::Clock::now();
        engine->request(eng::RequestView { "textDocument/hover", &params, file, {} }, message, [&](eng::Answer given) { answer = std::move(given); });
        expect(!answer.has_value()) << "clangd has not accepted requests: the request waits";
        const auto deadline = engine->next_deadline();
        expect(fatal(deadline.has_value()));
        expect(*deadline <= asked + 1s + 100ms) << "the engine wakes by the request's limit";
        std::this_thread::sleep_until(asked + 1s + 50ms);
        engine->handle_timers();
        expect(fatal(answer.has_value())) << "answered at its limit";
        expect(answer->kind == eng::Answer::Kind::unavailable) << "unavailable: the next engine answers it";
        expect(std::ranges::find(host.events, std::string { "request-timeout" }) != host.events.end());
        engine->shut_down();
        fs::remove_all(root);
    };

    "a file whose build runs far past its own history while the editor waits is spinning"_test = [] {
        // import-hang plan §4: the budget is five times the file's last build, never under 20 s.
        using namespace std::chrono_literals;
        const auto t0 = cld::GuardClock::now();
        const std::string uri { "file:///p/src/main.cpp" };
        cld::SpinWatch watch;
        // A first build with no history is never judged: it may be preparing modules.
        watch.sent(uri, 1, t0);
        watch.state(uri, "parsing includes, parsing main file", t0);
        watch.asked(uri, t0 + 1s);
        expect(watch.check(t0 + 10min).empty()) << "no history, no verdict";
        expect(!watch.next_due().has_value());
        watch.state(uri, "idle", t0 + 10min + 50ms);   // it did finish; history is 10 min + 50 ms

        cld::SpinWatch quick;
        quick.sent(uri, 1, t0);
        quick.state(uri, "parsing includes, parsing main file", t0);
        quick.state(uri, "idle", t0 + 10ms);   // history: 10 ms, so the budget is the 20 s floor
        quick.sent(uri, 2, t0 + 1s);           // `import hello.`
        quick.asked(uri, t0 + 1s + 1ms);       // a request sent before clangd says it started
        quick.state(uri, "parsing includes, parsing main file", t0 + 1s + 5ms);
        expect(quick.next_due() == t0 + 1s + 5ms + 20s);
        expect(quick.check(t0 + 20s).empty()) << "within its budget";
        const auto spins = quick.check(t0 + 1s + 5ms + 20s);
        expect(fatal(spins.size() == 1u));
        expect(spins[0].uri == uri && spins[0].textHash == 2u) << "the text being built is the one remembered";
        expect(spins[0].budget == 20s);
        expect(quick.check(t0 + 1h).empty()) << "reported once per build";
        expect(!quick.next_due().has_value());

        // Nothing waiting on the build: no verdict and no deadline, so the event loop is not woken for it.
        cld::SpinWatch idle;
        idle.sent(uri, 1, t0);
        idle.state(uri, "parsing main file", t0);
        idle.state(uri, "idle", t0 + 10ms);
        idle.state(uri, "parsing main file", t0 + 1s);
        expect(idle.check(t0 + 1h).empty() && !idle.next_due().has_value());

        // A file that always takes long keeps a budget of five times what it takes.
        cld::SpinWatch heavy;
        heavy.sent(uri, 1, t0);
        heavy.state(uri, "parsing main file", t0);
        heavy.state(uri, "idle", t0 + 15s);
        heavy.sent(uri, 2, t0 + 20s);
        heavy.state(uri, "parsing main file", t0 + 20s);
        heavy.sent(uri, 3, t0 + 21s);
        expect(heavy.check(t0 + 20s + 74s).empty()) << "75 s is its budget";
        expect(heavy.check(t0 + 20s + 75s).size() == 1u);

        // A restart builds nothing yet; forgetting a file drops it.
        quick.restarted();
        expect(!quick.next_due().has_value());
        heavy.forget(uri);
        expect(heavy.check(t0 + 10h).empty());
    };

    "a clangd that answers nothing and uses no CPU is stuck, and a busy one is not"_test = [] {
        using namespace std::chrono_literals;
        const auto t0 = cld::GuardClock::now();
        cld::StuckWatch watch { 5s };
        watch.suspect(t0, std::nullopt);
        expect(!watch.watching()) << "no CPU reading, nothing to go on";
        watch.suspect(t0, 10.0);
        expect(watch.watching() && watch.due() == t0 + 5s);
        watch.suspect(t0 + 1s, 99.0);
        expect(watch.started() == t0) << "a watch already running is not restarted";
        expect(!watch.check(t0 + 4s, 10.0).stuck && watch.watching()) << "not due yet: still watching";
        const auto idle = watch.check(t0 + 5s, 10.1);
        expect(idle.stuck && !watch.watching()) << "0.1 s of CPU in 5 s";
        watch.suspect(t0, 10.0);
        expect(!watch.check(t0 + 5s, 14.0).stuck) << "a core busy: compiling, not stuck";
        watch.suspect(t0, 10.0);
        expect(!watch.check(t0 + 5s, std::nullopt).stuck && !watch.watching()) << "no reading at the end: nothing proven";
        watch.suspect(t0, 10.0);
        watch.clear();
        expect(!watch.watching()) << "an answer ends the watch";

        // The engine: a request clangd holds on to, with its CPU flat, gets it restarted; with its CPU
        // climbing, it is left to finish; with no CPU reading (Windows), nothing is decided and the
        // event loop is not woken again and again for it.
        namespace fs = mcppls::platform::fs;
        enum class Cpu { flat, climbing, unreadable };
        for (const Cpu cpu : { Cpu::flat, Cpu::climbing, Cpu::unreadable }) {
            const bool stuck { cpu == Cpu::flat };
            const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
                std::format("mcppls-test-stuck-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
            (void)fs::create_directories(root);
            const std::string executable { mcppls::base::join_path(root, "clangd") };
            (void)fs::write_file(executable, "pretend-clangd");
            auto shared = std::make_shared<UnansweringProcess::Shared>();
            const auto began = eng::Clock::now();
            shared->cpu = [cpu, began]() -> std::optional<double> {
                if (cpu == Cpu::unreadable) return std::nullopt;
                return cpu == Cpu::flat ? 1.0 : 1.0 + std::chrono::duration<double>(eng::Clock::now() - began).count();
            };
            cld::Options options;
            options.executable = executable;
            options.version = "23.1.0";
            options.stuckAfter = 200ms;
            options.stuckWatch = 300ms;
            options.processFactory = [shared] { return std::make_unique<UnansweringProcess>(shared); };
            RecordingHost host { root };
            auto engine = cld::make_engine(std::move(options));
            engine->start(host);
            host.pump(*engine);
            engine->apply(nullptr);
            expect(fatal(engine->status().accepting)) << "handshake done, no plan: serving";
            const std::string file { mcppls::base::join_path(root, "a.cpp") };
            const Json params { { "textDocument", Json { { "uri", mcppls::base::path_to_uri(file) } } }, { "position", Json { { "line", 0 }, { "character", 0 } } } };
            const Json message { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "textDocument/hover" }, { "params", params } };
            std::optional<eng::Answer> answer;
            engine->request(eng::RequestView { "textDocument/hover", &params, file, {} }, message, [&](eng::Answer given) { answer = std::move(given); });
            // The event loop, for longer than the watch takes: wake at each deadline the engine names, and
            // often enough to hand back what its threads sent (a real loop is woken by those). A deadline
            // already past on every turn is an engine keeping its loop spinning.
            const auto end = eng::Clock::now() + 2s;
            int overdue { 0 };
            while (eng::Clock::now() < end && shared->starts < 2) {
                const auto now = eng::Clock::now();
                const auto next = engine->next_deadline();
                if (next && *next <= now) ++overdue;
                std::this_thread::sleep_until(std::min({ next.value_or(end), now + 20ms, end }));
                engine->handle_timers();
                host.pump(*engine);
            }
            expect(overdue < 50) << overdue << " turns with a deadline already past: the loop spins";
            const bool sawStuck { std::ranges::find(host.events, std::string { "engine-stuck" }) != host.events.end() };
            if (stuck) {
                expect(shared->starts == 2) << "restarted once";
                expect(sawStuck);
                expect(answer.has_value() && answer->kind == eng::Answer::Kind::unavailable) << "the request the old clangd held is answered by the next engine";
            } else {
                expect(shared->starts == 1) << "a busy clangd is not restarted";
                expect(!sawStuck);
                expect(!answer.has_value()) << "its request is still within its own timeout";
            }
            engine->shut_down();
            fs::remove_all(root);
        }
    };

    "a loader's message is told from clangd's own lines"_test = [] {
        expect(cld::loader_failure("clangd: /lib/aarch64-linux-gnu/libstdc++.so.6: version `GLIBCXX_3.4.30' not found (required by clangd)"));
        expect(cld::loader_failure("/opt/payload/clangd/bin/clangd: error while loading shared libraries: libz.so.1: cannot open shared object file: No such file or directory"));
        expect(cld::loader_failure("Error loading shared library libstdc++.so.6: No such file or directory (needed by /payload/clangd/bin/clangd)")) << "musl";
        expect(cld::loader_failure("Error relocating /payload/clangd/bin/clangd: __cxa_thread_atexit_impl: symbol not found")) << "musl";
        expect(cld::loader_failure("dyld[4242]: Library not loaded: @rpath/libz.1.dylib")) << "macOS";
        expect(!cld::loader_failure("E[10:31:02.100] Failed to build module greet; due to Failed to compile /p/greet.cppm"));
        expect(!cld::loader_failure("E[10:31:02.100] error while loading shared libraries is in this file's text")) << "clangd's own line, whatever it quotes";
        expect(!cld::loader_failure("I[10:31:02.100] clangd version 23.1.0"));
        expect(!cld::loader_failure(""));
    };

    // 0.0.3 plan B1: a clangd that dies before its handshake left the server's initialize unanswered
    // for good -- an editor stuck starting, with no features and no reason given.
    "a clangd that cannot run on this machine settles at once and is not restarted"_test = [] {
        using namespace std::chrono_literals;
        namespace fs = mcppls::platform::fs;
        for (const bool exitFirst : { false, true }) {
            const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
                std::format("mcppls-test-incompatible-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
            (void)fs::create_directories(root);
            const std::string executable { mcppls::base::join_path(root, "clangd") };
            (void)fs::write_file(executable, "pretend-clangd");
            auto shared = std::make_shared<DyingProcess::Shared>();
            shared->line = "clangd: /lib/aarch64-linux-gnu/libstdc++.so.6: version `GLIBCXX_3.4.30' not found (required by clangd)";
            shared->exitFirst = exitFirst;
            cld::Options options;
            options.executable = executable;
            options.version = "23.1.0";
            options.processFactory = [shared] { return std::make_unique<DyingProcess>(shared); };
            RecordingHost host { root };
            auto engine = cld::make_engine(std::move(options));
            engine->start(host);
            host.pump(*engine);
            const auto settledStatus = engine->status();
            expect(host.settled >= 1) << (exitFirst ? "exit first" : "line first") << ": the server's initialize is answered";
            expect(settledStatus.failed && settledStatus.state == "unavailable");
            const auto has = [&](std::string_view code) {
                return std::ranges::any_of(settledStatus.issues, [&](const auto& issue) { return issue.code == code; });
            };
            expect(has("engine-incompatible")) << "the status says why";
            expect(!has("engine-crashed")) << "one issue, not a crash as well";
            // Past the first restart's time: nothing restarts it.
            const auto end = eng::Clock::now() + 1500ms;
            while (eng::Clock::now() < end) {
                std::this_thread::sleep_for(50ms);
                engine->handle_timers();
                host.pump(*engine);
            }
            expect(shared->starts == 1) << "started " << shared->starts << " times";
            engine->shut_down();
            fs::remove_all(root);
        }
    };

    "a clangd that exits before its handshake twice still lets the server initialize"_test = [] {
        using namespace std::chrono_literals;
        namespace fs = mcppls::platform::fs;
        const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-early-exit-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        (void)fs::create_directories(root);
        const std::string executable { mcppls::base::join_path(root, "clangd") };
        (void)fs::write_file(executable, "pretend-clangd");
        auto shared = std::make_shared<DyingProcess::Shared>();   // no loader message: a crash, not an incompatibility
        cld::Options options;
        options.executable = executable;
        options.version = "23.1.0";
        options.processFactory = [shared] { return std::make_unique<DyingProcess>(shared); };
        RecordingHost host { root };
        auto engine = cld::make_engine(std::move(options));
        engine->start(host);
        host.pump(*engine);
        expect(host.settled == 0) << "one early exit may be a fluke: the restart gets its chance";
        const auto end = eng::Clock::now() + 3s;
        while (eng::Clock::now() < end && shared->starts < 2) {
            std::this_thread::sleep_for(50ms);
            engine->handle_timers();
            host.pump(*engine);
        }
        expect(fatal(shared->starts == 2)) << "restarted after the first exit";
        expect(host.settled == 1) << "the second early exit answers the server's initialize";
        const auto status = engine->status();
        expect(std::ranges::any_of(status.issues, [](const auto& issue) { return issue.code == "engine-crashed"; }));
        expect(!status.failed) << "two exits: still restarting";
        engine->shut_down();
        fs::remove_all(root);
    };

    "a module the engine has already built completes without a unit"_test = [] {
        using State = cld::Primer::State;
        cld::Primer primer;
        primer.set_limit(4);
        primer.set_modules({
            { "std", {}, "/prime/std.cpp" },
            { "base", { "std" }, "/prime/base.cpp" },
            { "base:part", { "std" }, "" },
            { "app", { "base", "base:part" }, "/prime/app.cpp" },
        });
        const std::vector<std::string> wanted { "app" };
        expect(primer.want(wanted) == 4u);
        // A warm start: std and base are cached, app changed since.
        const auto cached = [](const cld::PrimeModule& module) { return module.name == "std" || module.name == "base"; };
        const auto started = primer.start_ready(cached);
        expect(fatal(started.size() == 1u));
        expect(started.front()->name == "app") << "completions cascade to importers in the same call";
        expect(primer.state("std") == State::done && primer.state("base") == State::done && primer.state("base:part") == State::done);
        expect(primer.running() == 1u) << "nothing already built counts against the limit";
    };

    "the module more work waits on starts first"_test = [] {
        cld::Primer primer;
        primer.set_limit(1);
        primer.set_modules({
            { "std", {}, "/prime/std.cpp" },
            { "a-leaf", { "std" }, "/prime/a-leaf.cpp" },     // first by name, nothing above it
            { "z-root", { "std" }, "/prime/z-root.cpp" },     // last by name, two modules above it
            { "middle", { "z-root" }, "/prime/middle.cpp" },
            { "top", { "middle" }, "/prime/top.cpp" },
        });
        const std::vector<std::string> wanted { "top", "a-leaf" };
        expect(primer.want(wanted) == 5u);
        auto first = primer.start_ready();
        expect(fatal(first.size() == 1u));
        expect(first.front()->name == "std");
        primer.finish("std");
        const auto next = primer.start_ready();
        expect(fatal(next.size() == 1u));
        expect(next.front()->name == "z-root") << next.front()->name;
    };

    "a module that fails to compile dooms everything that imports it, transitively"_test = [] {
        const std::map<std::string, std::vector<std::string>, std::less<>> requires_ {
            { "std", {} },
            { "leaf", { "std" } },                          // does not import the failed module: safe
            { "xpkg.core", { "std" } },                     // the module that fails
            { "xpkg.executor", { "xpkg.core" } },            // imports it directly
            { "app", { "xpkg.executor" } },                 // imports it transitively, through app's own module
        };
        const auto doomed = cld::doomed_modules(requires_, "xpkg.core");
        expect(doomed.size() == 3u) << doomed.size();
        expect(doomed.contains("xpkg.core") && doomed.contains("xpkg.executor") && doomed.contains("app"));
        expect(!doomed.contains("leaf") && !doomed.contains("std")) << "a fault only affects where it is";
    };

    "a doomed module is resolved at once and never primed again"_test = [] {
        using State = cld::Primer::State;
        cld::Primer primer;
        primer.set_limit(4);
        primer.set_modules({
            { "std", {}, "/prime/std.cpp" },
            { "xpkg.core", { "std" }, "/prime/core.cpp" },
            { "app", { "xpkg.core" }, "/prime/app.cpp" },
        });
        const std::vector<std::string> wanted { "app" };
        expect(primer.want(wanted) == 3u);
        const auto started = primer.start_ready();
        expect(fatal(started.size() == 1u));
        expect(started.front()->name == "std");
        primer.finish("std");
        const auto coreStarted = primer.start_ready();
        expect(fatal(coreStarted.size() == 1u));
        expect(coreStarted.front()->name == "xpkg.core");
        expect(primer.running() == 1u);
        // clangd reported xpkg.core could not be built while it was still running: abandoning it
        // resolves it at once, so app is not blocked on a module known to be doomed.
        primer.abandon(std::vector<std::string> { "xpkg.core" });
        expect(primer.state("xpkg.core") == State::done && primer.running() == 0u);
        const auto next = primer.start_ready();
        expect(fatal(next.size() == 1u));
        expect(next.front()->name == "app") << "app may proceed once xpkg.core is resolved, however it resolved";
        // A graph reset (as after an engine restart) puts xpkg.core back to unwanted; abandoning it
        // again resolves it before anything asks for it, so it is never retried like an ordinary module.
        primer.reset();
        expect(primer.state("xpkg.core") == State::unwanted);
        primer.abandon(std::vector<std::string> { "xpkg.core" });
        expect(primer.state("xpkg.core") == State::done);
        expect(primer.want(wanted) == 1u) << "app is wanted; the traversal stops at xpkg.core, already resolved";
        expect(primer.state("app") == State::waiting && primer.state("std") == State::unwanted);
    };

    "restarts are capped, not merely spaced out"_test = [] {
        using namespace std::chrono_literals;
        cld::RestartGate gate;
        const auto t0 = cld::GuardClock::now();
        expect(!gate.at_cap(t0));
        gate.record(t0);
        gate.record(t0 + 1min);
        expect(!gate.at_cap(t0 + 2min)) << "two restarts in the window is not the cap yet";
        gate.record(t0 + 2min);
        expect(gate.at_cap(t0 + 3min)) << "a third restart within ten minutes reaches it";
        expect(!gate.at_cap(t0 + 11min)) << "the window ages out";
    };

    "clangd's log lines are forwarded at clangd's own severity"_test = [] {
        expect(cld::clangd_log_level("E[10:31:02.123] Failed to build module greet") == mcppls::base::log::Level::warning);
        expect(cld::clangd_log_level("I[10:31:02.123] Loaded compilation database") == mcppls::base::log::Level::debug);
        expect(cld::clangd_log_level("V[10:31:02.123] <-- textDocument/didOpen") == mcppls::base::log::Level::debug);
        expect(cld::clangd_log_level("D[10:31:02.123] some debug detail") == mcppls::base::log::Level::debug);
        expect(cld::clangd_log_level("a continuation line with no prefix at all") == mcppls::base::log::Level::info);
    };

    "the clangd traits table is keyed by version"_test = [] {
        const auto pinned = cld::traits_for_version("23.1.0");
        expect(pinned.tested && pinned.hangsOnUnresolvedImports && pinned.needsModulePreparation && pinned.needsModuleHints);
        expect(pinned.msvcStlNeedsNoAlignedAllocation && pinned.kitStdlibVersion == "23.1.0" && !pinned.importNavigation);
        const auto fixed = cld::traits_for_version("23.1.1");
        expect(!fixed.msvcStlNeedsNoAlignedAllocation) << "llvm-project#218152 is fixed in 23.1.1";
        expect(!fixed.tested && fixed.kitStdlibVersion == "23.1.1");
        const auto other = cld::traits_for_version("22.1.8");
        expect(other.hangsOnUnresolvedImports && other.needsModulePreparation && other.needsModuleHints && other.msvcStlNeedsNoAlignedAllocation && !other.tested)
            << "an unrecognized version gets every compensation";
    };

    "payload integrity checks size and sha256, and caches the hash"_test = [] {
        // usable plan W9.4.
        namespace fs = mcppls::platform::fs;
        const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-payload-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        (void)fs::create_directories(root);
        const std::string clangdPath { mcppls::base::join_path(root, "clangd") };
        const std::string kitJsonPath { mcppls::base::join_path(root, "kit.json") };
        const std::string content { "pretend-clangd-bytes" };
        const std::string kitContent { "{\"name\":\"k\"}" };
        (void)fs::write_file(clangdPath, content);
        (void)fs::write_file(kitJsonPath, kitContent);
        const std::string cacheFile { mcppls::base::join_path(root, "cache.json") };

        eng::PayloadPaths payload;
        payload.directory = root;
        payload.files.emplace("clangd", eng::PayloadFileIntegrity { content.size(), mcppls::base::sha256_hex(content) });
        payload.files.emplace("kit.json", eng::PayloadFileIntegrity { kitContent.size(), mcppls::base::sha256_hex(kitContent) });

        expect(eng::verify_payload_integrity(payload, cacheFile).empty()) << "both files match their manifest entry";
        expect(fs::is_regular_file(cacheFile)) << "a hash was computed and cached";

        // A no-op payload.files: nothing to check, regardless of what is on disk.
        eng::PayloadPaths empty;
        empty.directory = root;
        expect(eng::verify_payload_integrity(empty, cacheFile).empty());

        // Truncated: the size check alone catches it, no need to hash.
        eng::PayloadPaths truncated { payload };
        truncated.files.at("clangd").size = content.size() + 1;
        {
            const auto issues = eng::verify_payload_integrity(truncated, cacheFile);
            expect(fatal(issues.size() == 1u));
            expect(issues.front().path == "clangd" && issues.front().reason.find("expected") != std::string::npos) << issues.front().reason;
        }

        // Same size, wrong sha256 (a manifest that does not describe this file).
        eng::PayloadPaths wrongHash { payload };
        wrongHash.files.at("clangd").sha256 = std::string(64, '0');
        {
            const auto issues = eng::verify_payload_integrity(wrongHash, cacheFile);
            expect(fatal(issues.size() == 1u));
            expect(issues.front().path == "clangd");
        }

        // Missing entirely.
        eng::PayloadPaths missing { payload };
        missing.files.emplace("nonexistent", eng::PayloadFileIntegrity { 1, "x" });
        {
            const auto issues = eng::verify_payload_integrity(missing, cacheFile);
            expect(fatal(issues.size() == 1u));
            expect(issues.front().path == "nonexistent" && issues.front().reason == "is missing");
        }

        // The cache is trusted while size and modification time have not changed: tampering the
        // cached hash for an unmodified file (same stamp) changes the verdict, proving the second
        // call reused it instead of re-hashing the untouched file.
        expect(eng::verify_payload_integrity(payload, cacheFile).empty());
        auto tampered = fs::read_file(cacheFile);
        expect(fatal(tampered.has_value()));
        Json cacheJson = Json::parse(*tampered);
        cacheJson[clangdPath]["sha256"] = std::string(64, 'f');
        (void)fs::write_file(cacheFile, cacheJson.dump());
        {
            const auto issues = eng::verify_payload_integrity(payload, cacheFile);
            expect(fatal(issues.size() == 1u)) << "the tampered cache entry was trusted, not recomputed";
            expect(issues.front().path == "clangd");
        }

        fs::remove_all(root);
    };

    "engine request keys round-trip and tell roots and engines apart"_test = [] {
        // usable plan W9.1, overall design 5.1: the session parses this back out of a client response's
        // id to find which root's engine to forward it to, and to discard a stale generation.
        // `Json id { 42 }` would wrap the plain integer in a one-element array; `=` keeps it a scalar.
        const Json id = 42;
        const std::string key { orch::make_engine_request_key("/work/root-a", "clangd", 3, id) };
        const auto parsed = orch::parse_engine_request_key(key);
        expect(fatal(parsed.has_value()));
        expect(parsed->rootKey == "/work/root-a" && parsed->engineId == "clangd" && parsed->generation == 3 && parsed->engineRequestId == id);

        expect(orch::make_engine_request_key("/work/root-b", "clangd", 3, id) != key);
        expect(orch::make_engine_request_key("/work/root-a", "clice", 3, id) != key);
        expect(orch::make_engine_request_key("/work/root-a", "clangd", 4, id) != key);

        expect(!orch::parse_engine_request_key("not-a-key").has_value());
        expect(!orch::parse_engine_request_key("e:onlyonecolon").has_value());

        // A string id, and a root key that itself contains ':' (every Windows path does): fields are
        // length-prefixed rather than split on ':'. `Json { "s:5" }` would make an array.
        const Json stringId("s:5");
        const auto second = orch::parse_engine_request_key(orch::make_engine_request_key("C:/work/a:b", "c:d", 1, stringId));
        expect(fatal(second.has_value()));
        expect(second->rootKey == "C:/work/a:b" && second->engineId == "c:d" && second->generation == 1 && second->engineRequestId == stringId);
    };

    "build files, interactive methods and state names"_test = [] {
        expect(orch::is_build_file("mcpp.toml") && orch::is_build_file("CMakeLists.txt") && orch::is_build_file("x.cmake"));
        expect(!orch::is_build_file("main.cpp") && !orch::is_build_file("greet.cppm"));
        expect(cld::is_interactive("textDocument/hover") && cld::is_interactive("textDocument/definition"));
        expect(!cld::is_interactive("textDocument/didOpen") && !cld::is_interactive("workspace/symbol"));
        expect(orch::to_string(orch::State::ready) == "ready" && orch::to_string(orch::State::error) == "error");
        expect(orch::to_string(orch::State::degraded) == "degraded" && orch::to_string(orch::State::preparing) == "preparing");
    };

    return report();
}
