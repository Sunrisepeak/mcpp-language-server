// Save routing with the real workspace/index/planner and a core that starts no process.
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

namespace {
struct State {
    int plans { 0 };
    bool deferred { false };
    std::vector<std::string> stubs;
};

class Core : public e::Engine {
public:
    Core(std::shared_ptr<State> state, std::string stubs) : state_ { std::move(state) }, stubs_ { std::move(stubs) } {}
    std::string_view id() const override { return "save-test"; }
    std::span<const e::MethodCapability> methods() const override { return {}; }
    e::EngineTraits traits() const override { return {}; }
    e::EngineStatus status() const override { return { .name = "save-test", .role = "core", .state = "ready", .accepting = true }; }
    void start(e::Host&) override {}
    void shut_down() override {}
    void configure_plan(mcppls::normalize::PlanInput& input) const override { input.stubDirectory = stubs_; }
    void apply(const mcppls::normalize::EnginePlan* plan) override {
        if (!plan) return;
        ++state_->plans;
        state_->deferred = plan->standInsDeferred;
        state_->stubs = plan->stubModules;
    }
    void document(const e::DocumentEvent&) override {}
    void notify(const Json&) override {}
    void sources_changed() override {}
    bool claims(const e::RequestView&) const override { return false; }
    void request(const e::RequestView&, const Json&, e::Reply reply) override { reply(e::Answer {}); }
    void cancel(const Json&) override {}
    void client_response(int, const Json&, const Json&) override {}
    void handle_event(const Json&) override {}
    std::optional<e::Clock::time_point> next_deadline() const override { return std::nullopt; }
    void handle_timers() override {}
private:
    std::shared_ptr<State> state_;
    std::string stubs_;
};

class Sink : public o::ClientSink { void send(const Json&) override {} };

// A loaded model names its files canonically (project/model.cpp), and the workspace resolves a
// document's URI the same way. The temporary directory is an alias on macOS (/var is /private/var)
// and may be a short name on Windows, so the hand-built model starts from the canonical root too.
std::string canonical_root() {
    const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-save-plan-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)fs::create_directories(root);
    return fs::canonical_path(root);
}

struct Fixture {
    std::string root { canonical_root() };
    std::string source { mcppls::base::join_path(root, "main.cpp") };
    std::string uri { mcppls::base::path_to_uri(source) };
    std::shared_ptr<State> state { std::make_shared<State>() };
    std::shared_ptr<o::EventChannel> events { std::make_shared<o::EventChannel>() };
    Sink sink;
    std::unique_ptr<o::Workspace> workspace;
    std::string cache;
    int version { 1 };

    explicit Fixture(const std::string& text, std::vector<std::string> producerImports = {}) {
        (void)fs::write_file(source, text);
        o::SessionOptions options;
        options.engineFactories = [state = state, stubs = mcppls::base::join_path(root, "stubs")](const auto&, const auto&, bool) {
            o::EngineFactories factories;
            factories.core = [state, stubs] { return std::make_unique<Core>(state, stubs); };
            return factories;
        };
        workspace = std::make_unique<o::Workspace>(root, root, options, e::PayloadPaths {}, false, false, "", events, sink);
        cache = workspace->cache_directory();
        // The constructor's own cache sweep must have finished before the fixture removes its cache.
        if (auto event = events->pop_until(std::chrono::steady_clock::now() + std::chrono::seconds { 5 })) {
            if (event->kind == o::EventKind::cache_swept) workspace->handle_cache_swept(event->message);
        }
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
        mcppls::spec::TranslationUnit unit;
        unit.source = source;
        unit.workDirectory = root;
        unit.arguments = { "clang++", "-std=c++23", "-c", source };
        unit.requiredModules = std::move(producerImports);
        set.units.push_back(std::move(unit));
        model->database.sets.push_back(std::move(set));
        workspace->handle_model_loaded(0, std::move(model), false);
        workspace->did_open(Json { { "textDocument", { { "uri", uri }, { "languageId", "cpp" }, { "version", version }, { "text", text } } } });
    }

    ~Fixture() {
        workspace->shut_down();
        workspace.reset();
        fs::remove_all(cache);
        fs::remove_all(root);
    }

    void change(const std::string& text) {
        Json params { { "textDocument", { { "uri", uri }, { "version", ++version } } }, { "contentChanges", Json::array({ Json { { "text", text } } }) } };
        workspace->did_change(Json { { "method", "textDocument/didChange" }, { "params", params } }, params);
    }
    void save(const std::string& text) {
        (void)fs::write_file(source, text);
        Json params { { "textDocument", { { "uri", uri } } } };
        workspace->did_save(Json { { "method", "textDocument/didSave" }, { "params", params } }, params);
    }
    void tick(std::chrono::milliseconds wait) {
        std::this_thread::sleep_for(wait);
        workspace->handle_timers();
    }
};
}

int main() {
    using namespace mcppls::testing;
    using namespace std::chrono_literals;
    const auto arguments = mcppls::platform::env::arguments();
    if (arguments.size() == 1) {
        // The real workspace starts a cache sweep. Isolate the whole child process's cache,
        // including its worker threads, so this test never sweeps a user's workspaces.
        const std::string cache { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-save-tests-cache-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        auto environment = mcppls::platform::env::variables();
        std::erase_if(environment, [](const std::string& entry) { return entry.starts_with("MCPPLS_CACHE_DIR="); });
        environment.push_back("MCPPLS_CACHE_DIR=" + cache);
        mcppls::platform::toolrun::Request request;
        request.program = arguments.front();
        request.arguments = { "--isolated-save-tests" };
        request.purpose = "save-plan-test";
        request.bounds.hard = 30s;
        request.environment = std::move(environment);
        const auto result = mcppls::platform::toolrun::run(request);
        fs::remove_all(cache);
        if (!result) { std::println(std::cerr, "{}", result.error().message); return 1; }
        std::print("{}", result->output);
        std::print(std::cerr, "{}", result->error);
        return result->timedOut ? 1 : result->exitCode;
    }
    "saving an export body preserves the already planned module graph"_test = [] {
        Fixture f { "export module app;\nexport int answer() { return 1; }\n" };
        expect(fatal(f.state->plans == 1));
        const std::string edited { "export module app;\nexport int answer() { return 2; }\n" };
        f.change(edited);
        f.save(edited);
        f.tick(900ms);
        expect(f.state->plans == 1) << "a body save still reaches the engine without another whole-project plan";
    };
    "a saved new import still changes the plan"_test = [] {
        Fixture f { "export module app;\n" };
        expect(fatal(f.state->plans == 1));
        const std::string edited { "export module app;\nimport missing;\n" };
        f.change(edited);
        f.save(edited);
        f.tick(900ms);
        expect(f.state->plans == 2);
    };
    "saving a deferred import still supplies its stand-in"_test = [] {
        Fixture f { "int main() {}\n" };
        expect(fatal(f.state->plans == 1));
        const std::string edited { "import missing;\nint main() {}\n" };
        f.change(edited);
        f.tick(2100ms); // The draft plan defers the missing import that is not on disk yet.
        expect(fatal(f.state->plans == 2));
        expect(f.state->deferred && f.state->stubs.empty());
        f.save(edited);
        f.tick(900ms);
        expect(f.state->plans == 3) << "matching the draft's structure does not suppress the pending stand-in";
        expect(!f.state->deferred && std::ranges::find(f.state->stubs, "missing") != f.state->stubs.end());
    };
    "a save whose disk imports differ from the buffer still changes the plan"_test = [] {
        Fixture f { "export module app;\n" };
        expect(fatal(f.state->plans == 1));
        f.change("export module app;\n// body edit\n");
        f.save("export module app;\nimport missing;\n");
        f.tick(900ms);
        expect(f.state->plans == 2);
        expect(std::ranges::find(f.state->stubs, "missing") != f.state->stubs.end());
    };
    "saving still adds a scanned import omitted from the producer's graph"_test = [] {
        const std::string original { "export module app;\nimport first;\n#if 0\nimport second;\n#endif\n" };
        Fixture f { original, { "first" } };
        expect(fatal(f.state->plans == 1));
        expect(std::ranges::find(f.state->stubs, "second") == f.state->stubs.end());
        const std::string edited { original + "export int answer() { return 2; }\n" };
        f.change(edited);
        f.save(edited);
        f.tick(900ms);
        expect(f.state->plans == 2) << "an unchanged lexical graph alone is insufficient for F13's editing-source union";
        expect(std::ranges::find(f.state->stubs, "second") != f.state->stubs.end());
    };
    return report();
}
