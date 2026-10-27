// The answer layer in the real workspace (answer-layer design 2026-10-13), with a core engine that answers only when the
// test says: a slow engine's answer kept and given at the answer budget the next time; a request the person cancelled
// whose answer is kept all the same; a name kept in one file and given in another; a hover and a jump kept; an agent told
// where every answer came from; the answers kept between sessions; and none of it with mcppls.answer.cache off.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.base.uri;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.normalize.plan;
import mcppls.project.model;
import mcppls.project.detect;
import mcppls.spec.database;
import mcppls.toolchain.probe;
import mcppls.orchestrator.client;
import mcppls.orchestrator.workspace;
import mcpplibs.os;
import mcppls.toolrun;

namespace toolrun = mcppls::toolrun;

namespace base = mcppls::base;

namespace os = mcpplibs::os;

using Json = nlohmann::json;
namespace o = mcppls::orchestrator;
namespace e = mcppls::engine;
using namespace std::chrono_literals;

namespace {

// A core engine that holds every request until answer() is called for it.
struct Held {
    std::mutex mutex;
    std::map<std::string, e::Reply> pending;   // by the client's request id, dumped
    std::vector<std::string> asked;            // methods, in order
    std::vector<std::string> cancelled;
    bool ignoreCancellation { false };          // clangd may keep running and answer a cancelled request
    std::vector<std::string> detached;
    std::optional<Json> definitionResult;
    std::optional<Json> declarationResult;
    std::optional<Json> renameResult;           // a compiler's intentionally partial edit, when configured
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
        std::optional<Json> immediate;
        {
            const std::lock_guard lock { held_->mutex };
            held_->asked.emplace_back(view.method);
            const std::string id { message["id"].dump() };
            if (id.find("mcppls:answer:") != std::string::npos) held_->internal.push_back(message);
            if (view.method == "textDocument/rename" && held_->renameResult) immediate = held_->renameResult;
            else if (view.method == "textDocument/definition" && held_->definitionResult) immediate = held_->definitionResult;
            else if (view.method == "textDocument/declaration" && held_->declarationResult) immediate = held_->declarationResult;
            else held_->pending[id] = std::move(reply);
        }
        if (immediate) reply(e::Answer { e::Answer::Kind::result, std::move(*immediate) });
    }
    void cancel(const Json& id) override {
        e::Reply reply;
        {
            const std::lock_guard lock { held_->mutex };
            held_->cancelled.push_back(id.dump());
            if (held_->ignoreCancellation) return;
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
    std::error_code ignored;
    const std::string root { mcppls::base::join_path(base::spelled(os::dirs::temp()),
                                                     std::format("mcppls-answer-routing-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)std::filesystem::create_directories(root, ignored);
    return base::canonical_path(root);
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

    explicit Fixture(std::string at = canonical_root(), std::string cache = "on", bool ready = true, std::string agent = "cached",
                     std::function<std::unique_ptr<e::Engine>(std::shared_ptr<Held>)> coreFactory = {}) : root { std::move(at) } {
        o::SessionOptions options;
        options.answerCache = cache;
        options.answerAgent = agent;
        options.answerPrefetch = false;
        options.engineFactories = [held = held, coreFactory](const auto&, const auto&, bool) {
            o::EngineFactories factories;
            factories.core = [held, coreFactory]() -> std::unique_ptr<e::Engine> {
                return coreFactory ? coreFactory(held) : std::make_unique<Core>(held);
            };
            return factories;
        };
        workspace = std::make_unique<o::Workspace>(root, root, options, e::PayloadPaths {}, false, false, "", events, sink);
        if (auto event = events->pop_until(std::chrono::steady_clock::now() + std::chrono::seconds { 5 })) {
            if (event->kind == o::EventKind::cache_swept) workspace->handle_cache_swept(event->message);
        }
        workspace->start(Json { { "clientInfo", Json { { "name", "Visual Studio Code" } } },
                                { "capabilities", Json { { "experimental", Json { { "cxxModules", Json { { "version", 1 }, { "completionReady", ready } } } } } } } },
                         static_cast<bool>(coreFactory), false);
        load_model();
    }

    // The project: a.cpp and b.cpp, b.cpp compiled with `extra` too.
    void load_model(const std::vector<std::string>& extra = {}, bool engineInterface = false, std::string target = {}, int generation = 1) {
        std::error_code ignored;
        auto model = std::make_shared<mcppls::project::ProjectModel>();
        model->root = root;
        model->source = mcppls::project::SourceKind::build_database;
        if (!target.empty()) {
            model->profile.kind = "build-toolchain";
            model->profile.target = std::move(target);
        }
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
        if (engineInterface) {
            mcppls::spec::Set engineSet;
            engineSet.name = std::string { mcppls::spec::ENGINE_INTERFACE_SET };
            engineSet.configuration = std::string { mcppls::spec::BUILD_PROGRAM_CONFIGURATION };
            engineSet.hasIde = true;
            engineSet.toolchain = "test-clang";
            mcppls::spec::TranslationUnit unit;
            unit.source = mcppls::base::join_path(root, "engine/mcpp.cppm");
            unit.workDirectory = root;
            unit.role = mcppls::spec::Role::module_interface;
            unit.arguments = { "clang++", "-std=c++23", "-c", unit.source };
            (void)std::filesystem::create_directories(mcppls::base::parent_path(unit.source), ignored);
            (void)os::fs::write(unit.source, "export module mcpp;\nexport void cxxflag();\n");
            engineSet.units.push_back(std::move(unit));
            model->database.sets.push_back(std::move(engineSet));
        }
        // As the build tool's answer to the load start() began (generation 1); a later one replaces it.
        workspace->handle_model_loaded(generation, std::move(model));
    }

    ~Fixture() {
        workspace->shut_down();
        workspace.reset();
    }

    std::string uri(std::string_view name) const { return mcppls::base::path_to_uri(mcppls::base::join_path(root, name)); }

    void open(std::string_view name, std::string_view text) {
        (void)os::fs::write(mcppls::base::join_path(root, name), text);
        versions[std::string { name }] = 1;
        workspace->did_open(Json { { "textDocument", { { "uri", uri(name) }, { "languageId", "cpp" }, { "version", 1 }, { "text", std::string { text } } } } });
    }

    // A request at line/character; its id.
    Json ask(std::string_view method, std::string_view name, int line, int character, std::string idPrefix = "") {
        const Json id = idPrefix.empty() ? Json(nextId++) : Json(std::format("{}{}", idPrefix, nextId++));
        Json params { { "textDocument", { { "uri", uri(name) } } }, { "position", { { "line", line }, { "character", character } } } };
        if (method == "textDocument/rename") params["newName"] = "flag";
        workspace->route_client_request(Json { { "jsonrpc", "2.0" }, { "id", id }, { "method", std::string { method } }, { "params", params } });
        return id;
    }

    // The engine answers the request it holds under `id` (the client's) with `result`.
    bool answer(const Json& id, Json result, e::Answer::Kind kind = e::Answer::Kind::result) {
        e::Reply reply;
        {
            const std::lock_guard lock { held->mutex };
            const auto it = held->pending.find(id.dump());
            if (it == held->pending.end()) return false;
            reply = std::move(it->second);
            held->pending.erase(it);
        }
        reply(e::Answer { kind, std::move(result) });
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

Json definition_location(const std::string& uri, int shape = 0) {
    const Json range { { "start", Json { { "line", 0 }, { "character", 0 } } },
                       { "end", Json { { "line", 0 }, { "character", 7 } } } };
    const Json location { { "uri", uri }, { "range", range } };
    if (shape == 1) return Json::array({ location });
    if (shape == 2) return Json::array({ Json { { "targetUri", uri }, { "targetRange", range }, { "targetSelectionRange", range } } });
    return location;
}

Json rename_edit(const std::string& uri) {
    return Json { { "changes", Json { { uri, Json::array({ Json {
        { "range", Json { { "start", Json { { "line", 0 }, { "character", 10 } } },
                          { "end", Json { { "line", 0 }, { "character", 17 } } } } }, { "newText", "flag" }
    } }) } } } };
}

// A constant, not a std::string: nothing is allocated before main (the platform runtime is set up there).
constexpr std::string_view A { "#include <x>\nvoid f() {\n    doc.size();\n    std::vector<int> v;\n}\n" };   // `doc.` at 2:8, `std::` at 3:9

} // namespace

int main() {
    std::error_code ignored;
    using namespace mcppls::testing;

    const auto arguments = os::env::arguments();
    if (arguments.size() == 1) {
        // The workspaces keep their answers under the cache: isolated, so a user's cache is never touched.
        const std::string cache { mcppls::base::join_path(base::spelled(os::dirs::temp()),
                                                          std::format("mcppls-answer-tests-cache-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        auto environment = os::env::variables();
        std::erase_if(environment, [](const std::string& entry) { return entry.starts_with("MCPPLS_CACHE_DIR="); });
        environment.push_back("MCPPLS_CACHE_DIR=" + cache);
        mcppls::toolrun::Request request;
        request.program = arguments.front();
        request.arguments = { "--isolated-answer-tests" };
        request.purpose = "answer-routing-test";
        request.bounds.hard = 120s;
        request.environment = std::move(environment);
        const auto result = mcppls::toolrun::run(request);
        std::filesystem::remove_all(cache, ignored);
        if (!result) {
            std::println(std::cerr, "{}", result.error().message);
            return 1;
        }
        std::print("{}", result->out);
        std::print(std::cerr, "{}", result->err);
        return result->timed_out ? 1 : toolrun::exit_code(result->status);
    }
    "watched source acknowledgement covers successful run-start inputs, not failures or later disk edits"_test = [] {
        for (const int scenario : { 0, 1, 2, 3, 4, 5 }) {
            const bool failed { scenario == 1 }, restored { scenario == 2 };
            Fixture f;
            const auto source = [](std::string_view hidden, int body) {
                return std::format("import active;\n#if 0\nimport {};\n#endif\nint value() {{ return {}; }}\n", hidden, body);
            };
            const std::string path { mcppls::base::join_path(f.root, "a.cpp") };
            const std::string initial { scenario == 5 ? "#define SELECT(x) x\nimport SELECT(active);\nint value() { return 0; }\n"
                : scenario >= 3 ? "#if 1\nimport active;\n#endif\nint value() { return 0; }\n"
                                : "import active;\nint value() { return 0; }\n" };
            expect(os::fs::write(path, initial).has_value());
            expect(os::fs::write(mcppls::base::join_path(f.root, "b.cpp"), "export module active;\n").has_value());
            const auto loaded = [&](int generation, bool success) {
                auto model = std::make_shared<mcppls::project::ProjectModel>();
                model->root = f.root;
                model->detected = mcppls::project::SourceKind::build_database;
                if (success) {
                    model->source = mcppls::project::SourceKind::build_database;
                    model->watch = { "a.cpp" };
                    mcppls::spec::Set set;
                    set.name = "app";
                    for (const char* name : { "a.cpp", "b.cpp" }) {
                        mcppls::spec::TranslationUnit unit;
                        unit.source = mcppls::base::join_path(f.root, name);
                        unit.workDirectory = f.root;
                        unit.arguments = { "clang++", "-std=c++23", "-c", unit.source };
                        if (std::string_view { name } == "a.cpp") unit.requiredModules = { "active" };
                        else unit.providedModules = { { "active", "" } };
                        set.units.push_back(std::move(unit));
                    }
                    model->database.sets.push_back(std::move(set));
                } else model->issues.emplace_back("producer-failed", "the controlled producer returned no model");
                f.workspace->handle_model_loaded(generation, std::move(model));
            };
            const auto watched = [&] {
                f.workspace->handle_watched_files(Json::array({ Json {
                    { "uri", f.uri("a.cpp") }, { "type", 2 }
                } }));
            };
            loaded(1, true);
            if (scenario == 5) {
                watched();
                f.tick(1600ms);
                expect(fatal(f.workspace->report()["state"] == "loading"));
                loaded(2, true); // SELECT(active) really imports active; this answer is valid.
                expect(os::fs::write(path, "#define SELECT(x) x\nimport SELECT(other);\nint value() { return 0; }\n").has_value());
                watched();
                f.tick(1600ms);
                expect(f.workspace->report()["state"] == "loading")
                    << "the same macro definition with another import argument needs a new producer answer";
                continue;
            }
            if (scenario >= 3) {
                if (scenario == 4) {
                    // The source's active dependency exactly matches the producer's model.
                    // A successful refresh establishes a baseline for that same structure.
                    watched();
                    f.tick(1600ms);
                    expect(fatal(f.workspace->report()["state"] == "loading"));
                    loaded(2, true);
                }
                expect(os::fs::write(path, "#if 0\nimport active;\n#endif\nint value() { return 0; }\n").has_value());
                watched();
                f.tick(1600ms);
                expect(f.workspace->report()["state"] == "loading")
                    << "disabling the import changes effective dependencies even though lexical names still match the old model; baseline=" << (scenario == 4);
                continue;
            }
            expect(os::fs::write(path, source("hidden.alpha", 1)).has_value());
            watched();
            const auto beforeRun = base::file_stamp(path);
            f.tick(1600ms); // The existing producer debounce, not a status hold.
            expect(fatal(f.workspace->report()["state"] == "loading"));
            if (!failed) {
                // A checkout changes disk after the run began, before its watch reaches us.
                expect(os::fs::write(path, source("hidden.beta", 2)).has_value());
                if (restored) {
                    expect(os::fs::write(path, source("hidden.alpha", 1)).has_value());
                    expect(fatal(base::file_stamp(path) != beforeRun)) << "the A -> B -> A control changed the original file stamp";
                }
            }
            loaded(2, !failed);
            expect(os::fs::write(path, source(failed || restored ? "hidden.alpha" : "hidden.beta", 3)).has_value());
            watched();
            f.tick(1600ms);
            expect(f.workspace->report()["state"] == "loading")
                << (failed ? "a failed answer cannot acknowledge this structure: retry on the next body save"
                           : restored ? "a changed stamp cannot acknowledge A -> B -> A while the producer ran"
                                      : "a run cannot acknowledge the later disk structure it did not start with");
            loaded(3, true);
            expect(os::fs::write(path, source(failed || restored ? "hidden.alpha" : "hidden.beta", 4)).has_value());
            watched();
            f.tick(1600ms);
            expect(f.workspace->report()["state"] != "loading") << "a valid unchanged answer acknowledges the run-start structure";
        }
    };

    "a model's status is published only after its synchronous engine apply commits"_test = [] {
        struct Probe {
            Sink* sink { nullptr };
            bool notifyDuringApply { false };
            unsigned callbacks { 0 };
            unsigned commits { 0 };
            std::vector<Json> intermediate;
        };
        // clangd's apply calls update_quarantine_issue_ -> host.status_changed()
        // before updating the primer and scheduling the new plan's restart. Model that
        // permitted reentrant callback, without inventing a timer or an engine failure.
        class ApplyingCore : public Core {
        public:
            ApplyingCore(std::shared_ptr<Held> held, std::shared_ptr<Probe> probe)
                : Core { std::move(held) }, probe_ { std::move(probe) } {}
            void start(e::Host& host) override {
                host_ = &host;
                Core::start(host);
            }
            void apply(const mcppls::normalize::EnginePlan* plan) override {
                if (plan == nullptr || probe_->sink == nullptr) return;
                if (probe_->notifyDuringApply) {
                    const auto before = probe_->sink->sent.size();
                    ++probe_->callbacks;
                    host_->status_changed();
                    for (std::size_t i { before }; i < probe_->sink->sent.size(); ++i) {
                        const auto& message = probe_->sink->sent[i];
                        if (message.value("method", std::string {}) == "cxxModules/status") {
                            probe_->intermediate.push_back(message["params"]);
                        }
                    }
                }
                ++probe_->commits;
            }
        private:
            std::shared_ptr<Probe> probe_;
            e::Host* host_ { nullptr };
        };
        for (const bool reentrant : { false, true }) {
            auto probe = std::make_shared<Probe>();
            probe->notifyDuringApply = reentrant;
            Fixture f { canonical_root(), "off", true, "cached",
                [probe](std::shared_ptr<Held> held) -> std::unique_ptr<e::Engine> {
                    return std::make_unique<ApplyingCore>(std::move(held), probe);
                } };
            f.workspace->allow_status_notifications();
            // Use the existing explicit refresh API: its asynchronous response is delivered
            // below as generation 2. No sleep or status coalescing timeout is involved.
            f.workspace->apply_build_program();
            expect(f.workspace->report()["state"] == "loading") << "the replacement model is being described";
            f.sink.sent.clear();
            probe->sink = &f.sink;
            f.load_model({}, false, "aarch64-unknown-linux-gnu", 2);
            expect(probe->commits == 1u) << "the model actually reached its engine";
            expect(probe->callbacks == (reentrant ? 1u : 0u)) << "the callback control was exercised";
            expect(probe->intermediate.empty())
                << "a synchronous apply callback exposed the new profile before engine commit: " << Json(probe->intermediate).dump();
            const auto statuses = f.sink.notifications("cxxModules/status");
            expect(statuses > 0u) << "the committed model still needs a status notification";
            const auto last = std::ranges::find_if(f.sink.sent.rbegin(), f.sink.sent.rend(), [](const Json& message) {
                return message.value("method", std::string {}) == "cxxModules/status";
            });
            if (last != f.sink.sent.rend()) {
                expect((*last)["params"]["profile"]["target"] == "aarch64-unknown-linux-gnu");
            }
            // No async work or elapsed-time hold makes this assertion pass: both cases
            // use the same completed apply, with only the real callback pattern toggled.
            probe->sink = nullptr;
        }
    };
    "rename refuses an engine-interface owner even when the compiler edits only project references"_test = [] {
        for (int shape { 0 }; shape < 3; ++shape) {
            Fixture f;
            f.load_model({}, true);
            f.open("a.cpp", "void f(){ cxxflag(); cxxflag(); }\n");
            f.held->renameResult = rename_edit(f.uri("a.cpp"));
            const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
            // The old product goes straight to rename and receives this real partial WorkspaceEdit.
            // The fixed product instead holds definition, whose owner is the immutable interface.
            if (f.held->pending.contains(id.dump())) {
                expect(f.held->asked.back() == "textDocument/definition");
                expect(f.answer(id, definition_location(f.uri("engine/mcpp.cppm"), shape)));
            }
            const auto response = f.sink.response(id);
            expect(fatal(response.has_value()));
            expect(response->value("error", Json::object()).value("code", 0) == -32803) << response->dump();
            expect(std::ranges::count(f.held->asked, "textDocument/rename") == 0);
        }
    };

    "rename ownership permits project and writable dependency definitions and retains the edit guard"_test = [] {
        for (const std::string owner : { "a.cpp", "dependency.cppm" }) {
            Fixture f;
            f.load_model({}, true);
            f.open("a.cpp", "void f(){ cxxflag(); }\n");
            f.held->renameResult = rename_edit(f.uri("a.cpp"));
            const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
            expect(f.answer(id, definition_location(f.uri(owner), owner == "a.cpp" ? 1 : 2)));
            expect(f.held->asked.back() == "textDocument/declaration");
            expect(f.answer(id, definition_location(f.uri(owner), owner == "a.cpp" ? 2 : 1)));
            const auto response = f.sink.response(id);
            expect(fatal(response.has_value()));
            expect(response->contains("result") && (*response)["result"] == *f.held->renameResult) << response->dump();
            expect(std::ranges::count(f.held->asked, "textDocument/rename") == 1);
        }
        Fixture f;
        f.load_model({}, true);
        f.open("a.cpp", "void f(){ cxxflag(); }\n");
        f.held->renameResult = rename_edit(f.uri("engine/mcpp.cppm"));
        const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
        expect(f.answer(id, definition_location(f.uri("a.cpp"))));
        expect(f.answer(id, definition_location(f.uri("a.cpp"))));
        const auto response = f.sink.response(id);
        expect(fatal(response.has_value()));
        expect(response->value("error", Json::object()).value("code", 0) == -32803) << response->dump();
    };

    "rename refuses a readonly declaration even when the definition is implemented in the project"_test = [] {
        Fixture f;
        f.load_model({}, true);
        f.open("a.cpp", "void f(){ cxxflag(); }\n");
        f.held->renameResult = rename_edit(f.uri("a.cpp"));
        const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
        expect(f.answer(id, definition_location(f.uri("a.cpp"))));
        expect(f.held->asked.back() == "textDocument/declaration");
        expect(f.answer(id, definition_location(f.uri("engine/mcpp.cppm"), 2)));
        const auto response = f.sink.response(id);
        expect(fatal(response.has_value()));
        expect(response->value("error", Json::object()).value("code", 0) == -32803) << response->dump();
        expect(std::ranges::count(f.held->asked, "textDocument/rename") == 0);
    };

    "rename ownership supports unchanged closed files and rejects disk or open-state changes"_test = [] {
        for (int stage { 0 }; stage < 2; ++stage) for (int mode { 0 }; mode < 4; ++mode) {
            Fixture f;
            f.load_model({}, true);
            const std::string path { mcppls::base::join_path(f.root, "a.cpp") };
            if (mode != 3) expect(os::fs::write(path, "void f(){ cxxflag(); }\n").has_value());
            f.held->renameResult = rename_edit(f.uri("a.cpp"));
            const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
            if (mode == 3) {
                const auto response = f.sink.response(id);
                expect(fatal(response.has_value()));
                expect(response->value("error", Json::object()).value("code", 0) == -32803);
                expect(f.held->asked.empty());
                continue;
            }
            if (stage == 1) expect(f.answer(id, definition_location(f.uri("a.cpp"))));
            if (mode == 1) expect(os::fs::write(path, "void f(){ cxxflag(); } // disk changed\n").has_value());
            if (mode == 2) f.open("a.cpp", "void f(){ cxxflag(); }\n");
            expect(f.answer(id, definition_location(f.uri("a.cpp"))));
            if (mode == 0 && stage == 0) expect(f.answer(id, definition_location(f.uri("a.cpp"))));
            const auto response = f.sink.response(id);
            expect(fatal(response.has_value()));
            if (mode == 0) {
                expect(response->contains("result") && (*response)["result"] == *f.held->renameResult) << response->dump();
                expect(std::ranges::count(f.held->asked, "textDocument/rename") == 1);
            } else {
                expect(response->value("error", Json::object()).value("code", 0) == -32803) << response->dump();
                expect(std::ranges::count(f.held->asked, "textDocument/rename") == 0);
            }
        }
    };

    "rename ownership handles fully synchronous definition declaration and rename callbacks"_test = [] {
        Fixture f;
        f.load_model({}, true);
        f.open("a.cpp", "void f(){ cxxflag(); }\n");
        f.held->definitionResult = definition_location(f.uri("a.cpp"));
        f.held->declarationResult = definition_location(f.uri("a.cpp"), 2);
        f.held->renameResult = rename_edit(f.uri("a.cpp"));
        const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
        const auto response = f.sink.response(id);
        expect(fatal(response.has_value()));
        expect(response->contains("result") && (*response)["result"] == *f.held->renameResult) << response->dump();
        expect(f.held->asked == std::vector<std::string> { "textDocument/definition", "textDocument/declaration", "textDocument/rename" });
        expect(f.held->pending.empty());
    };

    "rename ownership fails closed on unresolved, malformed, errored and cancelled definitions"_test = [] {
        for (int stage { 0 }; stage < 2; ++stage) for (int mode { 0 }; mode < 9; ++mode) {
            Fixture f;
            f.load_model({}, true);
            f.open("a.cpp", "void f(){ cxxflag(); }\n");
            f.held->renameResult = rename_edit(f.uri("a.cpp"));
            const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
            if (stage == 1) expect(f.answer(id, definition_location(f.uri("a.cpp"))));
            Json definition = nullptr;
            if (mode == 1) definition = Json::array();
            if (mode == 2) definition = Json { { "uri", f.uri("a.cpp") }, { "range", Json::object() } };
            if (mode == 3) definition = Json::array({ definition_location(f.uri("a.cpp")), Json { { "uri", 7 } } });
            if (mode == 7) definition = definition_location("https://example.invalid/symbol.cpp");
            if (mode == 8) {
                definition = definition_location(f.uri("a.cpp"), 2);
                definition[0]["targetSelectionRange"]["end"]["character"] = 8;
            }
            const auto kind = mode == 4 ? e::Answer::Kind::error : mode == 5 ? e::Answer::Kind::unavailable
                : mode == 6 ? e::Answer::Kind::cancelled : e::Answer::Kind::result;
            expect(f.answer(id, definition, kind));
            const auto response = f.sink.response(id);
            expect(fatal(response.has_value()));
            expect(response->value("error", Json::object()).value("code", 0) == (mode == 6 ? -32800 : -32803)) << response->dump();
            expect(std::ranges::count(f.held->asked, "textDocument/rename") == 0);
        }
    };

    "rename ownership cannot continue after cancellation, an edit, or a replacement model"_test = [] {
        for (int stage { 0 }; stage < 2; ++stage) for (int mode { 0 }; mode < 4; ++mode) {
            Fixture f;
            f.load_model({}, true);
            f.open("a.cpp", "void f(){ cxxflag(); }\n");
            f.held->renameResult = rename_edit(f.uri("a.cpp"));
            const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
            if (stage == 1) expect(f.answer(id, definition_location(f.uri("a.cpp"))));
            if (mode == 0) f.workspace->cancel(id);
            if (mode == 1) f.change("a.cpp", "void f(){ cxxflag(); } // edited\n");
            if (mode == 2) f.load_model({ "-DNEW_MODEL" }, true);
            if (mode == 3) {
                o::SessionOptions options;
                options.buildDiscovery = "off";
                options.discoverCompilers = false;
                f.workspace->reload_with_options(options, "", false);
                f.tick(1600ms);   // starts a new load generation, before accepting its async result
            }
            if (mode != 0) expect(f.answer(id, definition_location(f.uri("a.cpp"))));
            else expect(!f.answer(id, definition_location(f.uri("a.cpp"))));
            const auto response = f.sink.response(id);
            expect(fatal(response.has_value()));
            expect(response->value("error", Json::object()).value("code", 0) == (mode == 0 ? -32800 : -32803)) << response->dump();
            expect(std::ranges::count(f.held->asked, "textDocument/rename") == 0);
        }
    };

    "rename ownership rejects late results before the next timer tick in either lookup phase"_test = [] {
        Fixture f;
        f.load_model({}, true);
        f.open("a.cpp", "void f(){ cxxflag(); }\n");
        f.held->renameResult = rename_edit(f.uri("a.cpp"));
        const Json definition = f.ask("textDocument/rename", "a.cpp", 0, 13);
        const Json declaration = f.ask("textDocument/rename", "a.cpp", 0, 13);
        expect(f.answer(declaration, definition_location(f.uri("a.cpp"))));
        std::this_thread::sleep_for(10050ms);   // deliberately do not call Workspace::handle_timers
        for (const Json id : { definition, declaration }) {
            expect(f.answer(id, definition_location(f.uri("a.cpp"))));
            const auto response = f.sink.response(id);
            expect(fatal(response.has_value()));
            expect(response->value("error", Json::object()).value("code", 0) == -32803) << response->dump();
        }
        expect(std::ranges::count(f.held->asked, "textDocument/declaration") == 1);
        expect(std::ranges::count(f.held->asked, "textDocument/rename") == 0);
    };

    "rename cancellation retires the local job even when the compiler ignores cancellation"_test = [] {
        for (int stage { 0 }; stage < 2; ++stage) {
            Fixture f;
            f.load_model({}, true);
            f.open("a.cpp", "void f(){ cxxflag(); }\n");
            f.held->renameResult = rename_edit(f.uri("a.cpp"));
            f.held->ignoreCancellation = true;
            const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
            if (stage == 1) expect(f.answer(id, definition_location(f.uri("a.cpp"))));
            const auto requestsBefore = f.held->asked.size();
            f.workspace->cancel(id);
            const auto response = f.sink.response(id);
            expect(fatal(response.has_value()));
            expect(response->value("error", Json::object()).value("code", 0) == -32800) << response->dump();
            expect(f.held->pending.contains(id.dump())) << "the compiler still has the cancelled lookup";
            expect(f.answer(id, definition_location(f.uri("a.cpp")))) << "it can return a successful late result";
            expect(f.held->asked.size() == requestsBefore) << "the late result advances neither lookup nor rename";
            expect(std::ranges::count(f.held->asked, "textDocument/rename") == 0);
            expect(std::ranges::count_if(f.sink.sent, [&](const Json& message) {
                return message.contains("id") && message["id"] == id && !message.contains("method");
            }) == 1) << "cancellation answers the client exactly once";
        }
    };

    "rename ownership uses the existing definition deadline and leaves ordinary workspaces unchanged"_test = [] {
        Fixture f;
        f.load_model({}, true);
        f.open("a.cpp", "void f(){ cxxflag(); }\n");
        f.held->renameResult = rename_edit(f.uri("a.cpp"));
        const Json id = f.ask("textDocument/rename", "a.cpp", 0, 13);
        f.tick(6000ms);
        expect(f.answer(id, definition_location(f.uri("a.cpp"))));
        expect(f.held->asked.back() == "textDocument/declaration");
        f.tick(4050ms);   // the second lookup shares the first one's deadline
        const auto response = f.sink.response(id);
        expect(fatal(response.has_value()));
        expect(response->value("error", Json::object()).value("code", 0) == -32803) << response->dump();
        expect(!f.held->pending.contains(id.dump()));
        expect(std::ranges::count(f.held->asked, "textDocument/rename") == 0);
        Fixture ordinary;
        ordinary.open("a.cpp", "void f(){ cxxflag(); }\n");
        ordinary.held->renameResult = rename_edit(ordinary.uri("a.cpp"));
        const Json direct = ordinary.ask("textDocument/rename", "a.cpp", 0, 13);
        expect(ordinary.sink.response(direct).has_value());
        expect(std::ranges::count(ordinary.held->asked, "textDocument/definition") == 0);
        expect(std::ranges::count(ordinary.held->asked, "textDocument/rename") == 1);
    };

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

    "a complete list nothing changed under is given complete, so the editor filters it"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        const auto complete = [](std::initializer_list<std::string_view> labels) {
            Json list = items(labels);
            list["isIncomplete"] = false;
            return list;
        };
        const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8);
        expect(f.answer(first, complete({ "push_back", "size" })));
        f.tick(10ms);
        const Json elsewhere = f.ask("textDocument/completion", "a.cpp", 3, 9);   // the word's own reuse (A1) now holds this one
        expect(f.answer(elsewhere, complete({ "vector", "string" })));
        f.tick(10ms);
        const Json again = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(500ms);
        const auto served = f.sink.response(again);
        expect(fatal(served.has_value()));
        expect(labels_of(*served) == std::vector<std::string> { "push_back", "size" }) << served->dump();
        expect((*served)["result"]["isIncomplete"] == false) << served->dump();
    };

    "an answer that came in the background gives the word's next letter its complete list at once"_test = [] {
        Fixture f;
        f.open("a.cpp", "#include <x>\nvoid f() {\n    doc.\n}\n");
        const Json first = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(1100ms);   // the file's words went out; the engine's request goes on
        expect(fatal(f.sink.response(first).has_value()));
        Json list = items({ "size", "swap", "push_back" });
        list["isIncomplete"] = false;
        expect(f.answer(first, list));
        f.tick(10ms);
        f.change("a.cpp", "#include <x>\nvoid f() {\n    doc.s\n}\n");
        const Json next = f.ask("textDocument/completion", "a.cpp", 2, 9);
        const auto served = f.sink.response(next);
        expect(fatal(served.has_value())) << "answered without waiting";
        expect((*served)["result"]["isIncomplete"] == false) << served->dump();
        expect(labels_of(*served) == std::vector<std::string> { "size", "swap", "push_back" }) << "the engine's list; the editor filters it";
    };

    "where the engine answers within its budget, a kept list waits for it a little past its pace"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        const auto complete = [](std::initializer_list<std::string_view> labels) {
            Json list = items(labels);
            list["isIncomplete"] = false;
            return list;
        };
        for (const auto& [line, character] : { std::pair { 2, 8 }, std::pair { 3, 9 } }) {   // the engine's pace: 0.6 s
            const Json asked = f.ask("textDocument/completion", "a.cpp", line, character);
            f.tick(600ms);
            expect(f.answer(asked, complete({ "push_back", "size" })));
            f.tick(10ms);
        }
        const Json again = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(500ms);
        expect(!f.sink.response(again).has_value()) << "the kept list waits: the engine is about to answer";
        f.tick(150ms);
        expect(f.answer(again, complete({ "push_back", "size", "empty" })));
        const auto served = f.sink.response(again);
        expect(fatal(served.has_value()));
        expect(labels_of(*served) == std::vector<std::string> { "push_back", "size", "empty" }) << "the engine's own";
    };

    "K1 background completions teach the file's pace from the original request"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        const Json initial = f.ask("textDocument/completion", "a.cpp", 2, 8);
        expect(f.answer(initial, items({ "cached" })));
        f.tick(10ms);

        // These lists stay incomplete deliberately: A1 must not hide the answer-layer path.
        // Four slow completions replace EnginePace's four-entry history, including the initial
        // fast seed. Each answer arrives 1.25 s after its request, but just 0.15 s after detach.
        // The unchanged production limits are 0.45 s for a kept answer, 1 s for the regular
        // request, and 2.5 s for adaptation. There is ample room on both sides of the probe.
        for (int sample { 0 }; sample < 4; ++sample) {
            const Json asked = f.ask("textDocument/completion", "a.cpp", 2, 8);
            f.tick(1100ms);
            const auto fallback = f.sink.response(asked);
            expect(fatal(fallback.has_value())) << "the held engine request gave its K1 fallback";
            expect((*fallback)["result"]["isIncomplete"] == true);
            expect(f.held->pending.contains(asked.dump())) << "the same engine request is still running";
            f.tick(150ms);
            expect(f.answer(asked, items({ "cached" })));
            f.tick(10ms);
        }
        const Json measured = f.workspace->report()["answers"]["fromEngine"];
        expect(measured.value("maxMs", 0.0) >= 1000.0)
            << "engine latency includes the wait before detaching, not only the background tail: " << measured.dump();

        const Json probe = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(750ms);   // well past the old 450 ms fallback; well inside the learned ~1.4 s wait
        expect(!f.sink.response(probe).has_value()) << "background samples let the next request wait for its engine";
        expect(f.held->pending.contains(probe.dump())) << "one live engine request, not a replacement";
        Json authoritative = items({ "engine_current" });
        authoritative["isIncomplete"] = false;
        expect(f.answer(probe, authoritative));
        const auto response = f.sink.response(probe);
        expect(fatal(response.has_value()));
        expect(labels_of(*response) == std::vector<std::string> { "engine_current" }) << response->dump();
        expect((*response)["result"]["isIncomplete"] == false) << "only the engine marked this list complete";
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

    "an agent is told when mcppls answered without the compiler, and nothing for other requests"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        const Json hover = f.ask("textDocument/hover", "a.cpp", 2, 9, "k:");
        f.tick(2100ms);   // hover's budget: nothing kept, the engine has not answered
        const auto answered = f.sink.response(hover);
        expect(fatal(answered.has_value()));
        expect((*answered)["provenance"]["authoritative"] == false && (*answered)["provenance"]["source"] == "native") << answered->dump();
        const Json symbols = f.ask("textDocument/documentSymbol", "a.cpp", 0, 0, "k:");
        expect(f.answer(symbols, Json::array()));
        f.tick(10ms);
        const auto outline = f.sink.response(symbols);
        expect(fatal(outline.has_value()));
        expect(!outline->contains("provenance")) << outline->dump();
    };

    "an agent that asked for the engine's own answer gets neither a kept answer nor mcppls's own"_test = [] {
        Fixture f;
        f.open("a.cpp", A);
        const Json hover = f.ask("textDocument/hover", "a.cpp", 2, 9, "ka:");
        f.tick(2100ms);   // past hover's budget
        expect(!f.sink.response(hover).has_value()) << "it waits for the engine, up to the agent's own deadline";
        expect(f.answer(hover, Json { { "contents", Json { { "kind", "markdown" }, { "value", "size_t size()" } } } }));
        const auto answered = f.sink.response(hover);
        expect(fatal(answered.has_value()));
        expect((*answered)["provenance"]["authoritative"] == true) << answered->dump();
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
        // The editor is served as before (a little past the engine's pace here: its answers took up to 0.5 s).
        const Json editor = f.ask("textDocument/completion", "a.cpp", 2, 8);
        f.tick(700ms);
        expect(f.sink.response(editor).has_value());
    };

    "a hover and a jump are kept; a jump only while the file it lands in is as it was"_test = [] {
        Fixture f;
        f.open("a.cpp", "void f() {\n    doc.dump();\n}\n");   // `dump` at 1:8
        const std::string header { mcppls::base::join_path(f.root, "json.hpp") };
        (void)os::fs::write(header, "struct json { void dump(); };\n");
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
        (void)os::fs::write(header, "// moved\nstruct json {\n    void dump();\n};\n");
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
