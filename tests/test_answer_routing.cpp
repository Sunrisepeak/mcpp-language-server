// The answer layer in the real workspace (answer-layer design 2026-10-13), with a core engine that answers only when the
// test says: a slow engine's answer kept and given at the answer budget the next time; a request the person cancelled
// whose answer is kept all the same; a name kept in one file and given in another; a hover and a jump kept; an agent told
// where every answer came from; the answers kept between sessions; and none of it with mcppls.answer.cache off.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.base.uri;
import mcppls.platform.dirs;
import mcppls.platform.fs;
import mcppls.platform.env;
import mcppls.platform.toolrun;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.normalize.plan;
import mcppls.project.model;
import mcppls.project.detect;
import mcppls.spec.database;
import mcppls.toolchain.probe;
import mcppls.orchestrator.client;
import mcppls.orchestrator.workspace;

using Json = nlohmann::json;
namespace o = mcppls::orchestrator;
namespace e = mcppls::engine;
namespace fs = mcppls::platform::fs;
using namespace std::chrono_literals;

namespace {

// A core engine that holds every request until answer() is called for it.
struct Held {
    std::mutex mutex;
    std::map<std::string, e::Reply> pending;   // by the client's request id, dumped
    std::vector<std::string> asked;            // methods, in order
    std::vector<std::string> cancelled;
    std::vector<std::string> detached;
    std::vector<Json> internal;                // requests mcppls made itself (learning, prefetching)
};

class Core : public e::Engine {
public:
    explicit Core(std::shared_ptr<Held> held) : held_ { std::move(held) } {}
    std::string_view id() const override { return "held-core"; }
    std::span<const e::MethodCapability> methods() const override { return declared_; }
    e::EngineTraits traits() const override { return {}; }
    e::EngineStatus status() const override { return { .name = "held-core", .role = "core", .state = "ready", .accepting = true }; }
    void start(e::Host& host) override { host.engine_settled(id(), Json::object()); }
    void shut_down() override {}
    void configure_plan(mcppls::normalize::PlanInput&) const override {}
    void apply(const mcppls::normalize::EnginePlan*) override {}
    void document(const e::DocumentEvent&) override {}
    void notify(const Json&) override {}
    void sources_changed() override {}
    bool claims(const e::RequestView&) const override { return true; }
    void request(const e::RequestView& view, const Json& message, e::Reply reply) override {
        const std::lock_guard lock { held_->mutex };
        held_->asked.emplace_back(view.method);
        const std::string id { message["id"].dump() };
        if (id.find("mcppls:answer:") != std::string::npos) held_->internal.push_back(message);
        held_->pending[id] = std::move(reply);
    }
    void cancel(const Json& id) override {
        e::Reply reply;
        {
            const std::lock_guard lock { held_->mutex };
            held_->cancelled.push_back(id.dump());
            const auto it = held_->pending.find(id.dump());
            if (it == held_->pending.end()) return;
            reply = std::move(it->second);
            held_->pending.erase(it);
        }
        reply(e::Answer { e::Answer::Kind::cancelled, Json() });
    }
    void detach(const Json& id) override {
        const std::lock_guard lock { held_->mutex };
        held_->detached.push_back(id.dump());
    }
    void client_response(int, const Json&, const Json&) override {}
    void handle_event(const Json&) override {}
    std::optional<e::Clock::time_point> next_deadline() const override { return std::nullopt; }
    void handle_timers() override {}

private:
    std::shared_ptr<Held> held_;
    std::vector<e::MethodCapability> declared_ { { std::string { e::EVERY_METHOD }, e::Role::answer, 0 } };
};

class Sink : public o::ClientSink {
public:
    std::vector<Json> sent;
    void send(const Json& message) override { sent.push_back(message); }
    // The response to `id`, if one went out.
    std::optional<Json> response(const Json& id) const {
        for (const auto& message : sent) {
            if (message.contains("id") && message["id"] == id && !message.contains("method")) return message;
        }
        return std::nullopt;
    }
    std::size_t notifications(std::string_view method) const {
        return static_cast<std::size_t>(std::ranges::count_if(sent, [&](const Json& m) { return m.value("method", std::string {}) == method; }));
    }
};

std::string canonical_root() {
    const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
                                                     std::format("mcppls-answer-routing-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)fs::create_directories(root);
    return fs::canonical_path(root);
}

// A list the engine marks incomplete: the client asks again as the person types, so 0.0.14's own reuse of a complete
// list within its word (A1) leaves it to the answer layer.
Json items(std::initializer_list<std::string_view> labels) {
    Json list = Json::array();
    for (const auto label : labels) list.push_back(Json { { "label", std::string { label } }, { "kind", 2 }, { "filterText", std::string { label } } });
    return Json { { "isIncomplete", true }, { "items", list } };
}

std::vector<std::string> labels_of(const Json& response) {
    std::vector<std::string> labels;
    for (const auto& item : response["result"].value("items", Json::array())) labels.push_back(item.value("label", std::string {}));
    return labels;
}

struct Fixture {
    std::string root;
    std::shared_ptr<Held> held { std::make_shared<Held>() };
    std::shared_ptr<o::EventChannel> events { std::make_shared<o::EventChannel>() };
    Sink sink;
    std::unique_ptr<o::Workspace> workspace;
    std::map<std::string, int> versions;
    int nextId { 1 };

    explicit Fixture(std::string at = canonical_root(), std::string cache = "on", bool ready = true, std::string agent = "cached") : root { std::move(at) } {
        o::SessionOptions options;
        options.answerCache = cache;
        options.answerAgent = agent;
        options.answerPrefetch = false;
        options.engineFactories = [held = held](const auto&, const auto&, bool) {
            o::EngineFactories factories;
            factories.core = [held] { return std::make_unique<Core>(held); };
            return factories;
        };
        workspace = std::make_unique<o::Workspace>(root, root, options, e::PayloadPaths {}, false, false, "", events, sink);
        if (auto event = events->pop_until(std::chrono::steady_clock::now() + std::chrono::seconds { 5 })) {
            if (event->kind == o::EventKind::cache_swept) workspace->handle_cache_swept(event->message);
        }
        workspace->start(Json { { "clientInfo", Json { { "name", "Visual Studio Code" } } },
                                { "capabilities", Json { { "experimental", Json { { "cxxModules", Json { { "version", 1 }, { "completionReady", ready } } } } } } } },
                         false, false);
        load_model();
    }

    // The project: a.cpp and b.cpp, b.cpp compiled with `extra` too.
    void load_model(const std::vector<std::string>& extra = {}) {
        auto model = std::make_shared<mcppls::project::ProjectModel>();
        model->root = root;
        model->source = mcppls::project::SourceKind::build_database;
        mcppls::spec::Set set;
        set.name = "app";
        set.hasIde = true;
        set.toolchain = "test-clang";
        mcppls::toolchain::ToolchainFacts facts;
        facts.toolchain.family = mcppls::spec::Family::clang;
        facts.toolchain.driver = "clang++";
        facts.toolchain.target = "x86_64-unknown-linux-gnu";
        model->facts.emplace(set.toolchain, std::move(facts));
        for (const char* name : { "a.cpp", "b.cpp" }) {
            mcppls::spec::TranslationUnit unit;
            unit.source = mcppls::base::join_path(root, name);
            unit.workDirectory = root;
            unit.arguments = { "clang++", "-std=c++23" };
            if (std::string_view { name } == "b.cpp") unit.arguments.insert(unit.arguments.end(), extra.begin(), extra.end());
            unit.arguments.insert(unit.arguments.end(), { "-c", unit.source });
            set.units.push_back(std::move(unit));
        }
        model->database.sets.push_back(std::move(set));
        // As the build tool's answer to the load start() began (generation 1); a later one replaces it.
        workspace->handle_model_loaded(1, std::move(model));
    }

    ~Fixture() {
        workspace->shut_down();
        workspace.reset();
    }

    std::string uri(std::string_view name) const { return mcppls::base::path_to_uri(mcppls::base::join_path(root, name)); }

    void open(std::string_view name, const std::string& text) {
        (void)fs::write_file(mcppls::base::join_path(root, name), text);
        versions[std::string { name }] = 1;
        workspace->did_open(Json { { "textDocument", { { "uri", uri(name) }, { "languageId", "cpp" }, { "version", 1 }, { "text", text } } } });
    }

    // A request at line/character; its id.
    Json ask(std::string_view method, std::string_view name, int line, int character, std::string idPrefix = "") {
        const Json id = idPrefix.empty() ? Json(nextId++) : Json(std::format("{}{}", idPrefix, nextId++));
        Json params { { "textDocument", { { "uri", uri(name) } } }, { "position", { { "line", line }, { "character", character } } } };
        workspace->route_client_request(Json { { "jsonrpc", "2.0" }, { "id", id }, { "method", std::string { method } }, { "params", params } });
        return id;
    }

    // The engine answers the request it holds under `id` (the client's) with `result`.
    bool answer(const Json& id, Json result) {
        e::Reply reply;
        {
            const std::lock_guard lock { held->mutex };
            const auto it = held->pending.find(id.dump());
            if (it == held->pending.end()) return false;
            reply = std::move(it->second);
            held->pending.erase(it);
        }
        reply(e::Answer { e::Answer::Kind::result, std::move(result) });
        return true;
    }

    void change(std::string_view name, const std::string& text) {
        Json params { { "textDocument", { { "uri", uri(name) }, { "version", ++versions[std::string { name }] } } },
                      { "contentChanges", Json::array({ Json { { "text", text } } }) } };
        workspace->did_change(Json { { "method", "textDocument/didChange" }, { "params", params } }, params);
    }

    void tick(std::chrono::milliseconds wait) {
        std::this_thread::sleep_for(wait);
        workspace->handle_timers();
    }
};

const std::string A { "#include <x>\nvoid f() {\n    doc.size();\n    std::vector<int> v;\n}\n" };   // `doc.` at 2:8, `std::` at 3:9

} // namespace

int main() {
    using namespace mcppls::testing;
    const auto arguments = mcppls::platform::env::arguments();
    if (arguments.size() == 1) {
        // The workspaces keep their answers under the cache: isolated, so a user's cache is never touched.
        const std::string cache { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
                                                          std::format("mcppls-answer-tests-cache-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        auto environment = mcppls::platform::env::variables();
        std::erase_if(environment, [](const std::string& entry) { return entry.starts_with("MCPPLS_CACHE_DIR="); });
        environment.push_back("MCPPLS_CACHE_DIR=" + cache);
        mcppls::platform::toolrun::Request request;
        request.program = arguments.front();
        request.arguments = { "--isolated-answer-tests" };
        request.purpose = "answer-routing-test";
        request.bounds.hard = 120s;
        request.environment = std::move(environment);
        const auto result = mcppls::platform::toolrun::run(request);
        fs::remove_all(cache);
        if (!result) {
            std::println(std::cerr, "{}", result.error().message);
            return 1;
        }
        std::print("{}", result->output);
        std::print(std::cerr, "{}", result->error);
        return result->timedOut ? 1 : result->exitCode;
    }

    "a slow engine's answer is kept, and given at the answer budget the next time"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(1100ms);   // nothing kept: the 1 s budget, and the file's words
        expect(fatal(f.sink.response(first).has_value()));
        expect(f.answer(first, items({ "push_back", "size" }))) << "the engine's request went on after the budget";
        f.tick(10ms);
        expect(f.sink.notifications("cxxModules/completionReady") == 1u) << "nothing went out after the `.`: the list is ready now";
        // A line typed above it: the same place, by another line (0.0.14's same-word reuse no longer applies).
        f.change("a.cpp", "#include <x>\nvoid f() {\n    int x;\n    doc.size();\n    std::vector<int> v;\n}\n");
        const Json second = f.ask("textDocument/completion", "a.cpp", 3, 8);
        expect(!f.sink.response(second).has_value()) << "within the budget the engine may still answer";
        f.tick(500ms);
        const auto served = f.sink.response(second);
        expect(fatal(served.has_value()));
        expect(labels_of(*served) == std::vector<std::string> { "push_back", "size" }) << served->dump();
        expect((*served)["result"]["isIncomplete"] == true);
        // The engine's own answer, when it comes, is kept in place of the one given.
        expect(f.answer(second, items({ "push_back", "size", "empty" })));
        f.tick(10ms);
        expect(f.sink.notifications("cxxModules/completionReady") == 1u) << "a kept list went out: nothing to say";
        const Json report = f.workspace->report();
        expect(report["answers"]["answers"].value("completion cache:K1", 0) == 1) << report["answers"].dump();
        // Both differed from what went out: the file's words the first time, the kept list (one item fewer) the second.
        expect(report["answers"]["replaced"]["different"] == 2 && report["answers"]["replaced"]["itemsAdded"] == 3) << report["answers"].dump();
    };

    "a client that did not declare completionReady is not sent it"_test = [] {
        Fixture f { canonical_root(), "on", false };
        f.open("a.cpp", A);
        const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(1100ms);
        expect(fatal(f.sink.response(first).has_value()));
        expect(f.answer(first, items({ "push_back", "size" })));
        f.tick(10ms);
        expect(f.sink.notifications("cxxModules/completionReady") == 0u) << "the list waits for the next keystroke";
    };

    "once the person typed on, an answer is not called up again"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(1100ms);
        expect(fatal(f.sink.response(first).has_value()));
        f.change("a.cpp", "#include <x>\nvoid f() {\n    doc.si();\n    std::vector<int> v;\n}\n");
        expect(f.answer(first, items({ "push_back", "size" })));
        f.tick(10ms);
        expect(f.sink.notifications("cxxModules/completionReady") == 0u) << "the document is not the one asked about";
    };

    "a request the person cancelled keeps its answer for the next time"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        const Json asked = f.ask("textDocument/completion", "a.cpp", 3, 9);
        f.workspace->cancel(asked);
        const auto cancelled = f.sink.response(asked);
        expect(fatal(cancelled.has_value()));
        expect(cancelled->contains("error")) << "the client is told at once";
        expect(f.held->cancelled.empty() && f.held->detached.size() == 1u) << "the engine's request goes on";
        expect(f.answer(asked, items({ "vector", "string" })));
        f.tick(10ms);
        const Json again = f.ask("textDocument/completion", "a.cpp", 3, 9);
        f.tick(500ms);
        const auto served = f.sink.response(again);
        expect(fatal(served.has_value()));
        expect(labels_of(*served) == std::vector<std::string> { "vector", "string" }) << served->dump();
    };

    "a name kept in one file is given in another, and the same place asked again is answered at once"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        f.open("b.cpp", "void g() {\n    std::string s;\n}\n");   // `std::` at 1:9
        const Json first = f.ask("textDocument/completion", "a.cpp", 3, 9);
        f.tick(1100ms);
        expect(f.answer(first, items({ "vector", "string" })));
        f.tick(10ms);
        const Json other = f.ask("textDocument/completion", "b.cpp", 1, 9);
        // The same place again while the engine still works on it: no second request, the kept answer at once.
        const Json again = f.ask("textDocument/completion", "b.cpp", 1, 9);
        expect(fatal(f.sink.response(again).has_value())) << "answered without waiting";
        expect(labels_of(*f.sink.response(again)) == std::vector<std::string> { "vector", "string" });
        f.tick(500ms);
        expect(fatal(f.sink.response(other).has_value()));
        expect(labels_of(*f.sink.response(other)) == std::vector<std::string> { "vector", "string" });
    };

    "an agent is told where every answer came from"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8, "k:");
        expect(f.answer(first, items({ "push_back" })));
        f.tick(10ms);
        const auto engine = f.sink.response(first);
        expect(fatal(engine.has_value()));
        expect((*engine)["provenance"]["authoritative"] == true) << engine->dump();
        const Json kept = f.ask("textDocument/completion", "a.cpp", 2, 8, "k:");
        f.tick(500ms);
        const auto served = f.sink.response(kept);
        expect(fatal(served.has_value()));
        expect((*served)["provenance"]["authoritative"] == false && (*served)["provenance"]["source"] == "cache:K1") << served->dump();
        // An agent that wants the engine's own answer waits for it.
        const Json fresh = f.ask("textDocument/completion", "a.cpp", 2, 8, "ka:");
        f.tick(500ms);
        expect(!f.sink.response(fresh).has_value());
        expect(f.answer(fresh, items({ "push_back", "size" })));
        expect((*f.sink.response(fresh))["provenance"]["authoritative"] == true);
        // The editor's own responses carry nothing of it.
        const Json editor = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(500ms);
        expect(fatal(f.sink.response(editor).has_value()));
        expect(!f.sink.response(editor)->contains("provenance"));
    };

    "with mcppls.answer.agent authoritative, an agent always waits for the engine"_test = [] {
        Fixture f { canonical_root(), "on", true, "authoritative" };
        f.open("a.cpp", A);
        const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8, "k:");
        expect(f.answer(first, items({ "push_back" })));
        f.tick(10ms);
        const Json again = f.ask("textDocument/completion", "a.cpp", 2, 8, "k:");
        f.tick(500ms);
        expect(!f.sink.response(again).has_value()) << "no kept answer stands in";
        expect(f.answer(again, items({ "push_back", "size" })));
        expect((*f.sink.response(again))["provenance"]["authoritative"] == true);
        // The editor is served as before.
        const Json editor = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(500ms);
        expect(f.sink.response(editor).has_value());
    };

    "a hover and a jump are kept; a jump only while the file it lands in is as it was"_test = [] {
        Fixture f;
        f.open("a.cpp", "void f() {\n    doc.dump();\n}\n");   // `dump` at 1:8
        const std::string header { mcppls::base::join_path(f.root, "json.hpp") };
        (void)fs::write_file(header, "struct json { void dump(); };\n");
        const Json hover = f.ask("textDocument/hover", "a.cpp", 1, 9);
        expect(f.answer(hover, Json { { "contents", Json { { "kind", "markdown" }, { "value", "void dump()" } } } }));
        const Json definition = f.ask("textDocument/definition", "a.cpp", 1, 9);
        const Json target = Json::array({ Json { { "uri", mcppls::base::path_to_uri(header) },
                                                 { "range", Json { { "start", Json { { "line", 0 }, { "character", 19 } } }, { "end", Json { { "line", 0 }, { "character", 23 } } } } } } });
        expect(f.answer(definition, target));
        f.tick(10ms);
        const Json hoverAgain = f.ask("textDocument/hover", "a.cpp", 1, 10);
        const Json definitionAgain = f.ask("textDocument/definition", "a.cpp", 1, 10);
        f.tick(500ms);
        const auto hovered = f.sink.response(hoverAgain);
        expect(fatal(hovered.has_value()));
        expect((*hovered)["result"]["contents"]["value"] == "void dump()" && (*hovered)["result"]["range"]["start"]["character"] == 8) << hovered->dump();
        const auto jumped = f.sink.response(definitionAgain);
        expect(fatal(jumped.has_value()));
        expect((*jumped)["result"][0]["range"]["start"]["character"] == 19);
        // The header moved its declaration: found again in its text.
        std::this_thread::sleep_for(20ms);
        (void)fs::write_file(header, "// moved\nstruct json {\n    void dump();\n};\n");
        const Json moved = f.ask("textDocument/definition", "a.cpp", 1, 10);
        f.tick(500ms);
        const auto found = f.sink.response(moved);
        expect(fatal(found.has_value()));
        expect((*found)["result"][0]["range"]["start"]["line"] == 2 && (*found)["result"][0]["range"]["start"]["character"] == 9) << found->dump();
    };

    "a plan that compiles another file otherwise makes every kept answer uncertain"_test = [] {
        for (const bool otherwise : { false, true }) {
            Fixture f;
            f.open("a.cpp", "void f() {\n    doc.dump();\n}\n");
            f.tick(10ms);
            const Json hover = f.ask("textDocument/hover", "a.cpp", 1, 9, "k:");
            expect(f.answer(hover, Json { { "contents", Json { { "kind", "markdown" }, { "value", "void dump()" } } } }));
            f.tick(10ms);
            f.load_model(otherwise ? std::vector<std::string> { "-DJSON_DIAGNOSTICS=1" } : std::vector<std::string> {});
            f.tick(100ms);
            const Json again = f.ask("textDocument/hover", "a.cpp", 1, 10, "k:");
            f.tick(500ms);
            const auto served = f.sink.response(again);
            expect(fatal(served.has_value()));
            expect((*served)["provenance"]["certainty"] == (otherwise ? "uncertain" : "certain")) << otherwise << served->dump();
        }
    };

    "answers are kept between sessions"_test = [] {
        const std::string root { canonical_root() };
        {
            Fixture f { root };
            f.open("a.cpp", A);
            const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8);
            expect(f.answer(first, items({ "push_back", "size" })));
            f.tick(10ms);
        }   // shut down: written
        Fixture f { root };
        f.open("a.cpp", A);
        const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(500ms);
        const auto served = f.sink.response(first);
        expect(fatal(served.has_value()));
        expect(labels_of(*served) == std::vector<std::string> { "push_back", "size" }) << served->dump();
    };

    "with mcppls.answer.cache off, nothing is kept"_test = [] {
        Fixture f { canonical_root(), "off" };
        f.open("a.cpp", A);
        const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8);
        expect(f.answer(first, items({ "push_back" })));
        f.tick(10ms);
        const Json second = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(500ms);
        expect(!f.sink.response(second).has_value()) << "waits for the engine as 0.0.14 did";
        expect(f.workspace->report()["answers"]["entries"] == 0);
    };

    return report();
}
