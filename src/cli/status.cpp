module mcppls.cli.status;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcpplibs.os;
import mcppls.base.path;
import mcppls.base.version;
import mcppls.base.payload;
import mcppls.base.home;
import mcppls.engine.payload;
import mcppls.orchestrator.instance;
import mcppls.project.model;
import mcppls.cli.options;

namespace os = mcpplibs::os;

namespace mcppls::cli {

namespace {

using Json = nlohmann::json;
using namespace mcpplibs;

// What this mcppls declares it can do, by name; an IDE turns a feature on by the name and ignores a
// name it does not know (S3 section 8.1).
const std::vector<std::string_view> CAPABILITIES { "status", "kit-install", "module-graph", "other-cpp-features" };

// The actions this mcppls offers, under each editor's own id (S3 section 8.2). The ids are mcppls's:
// an IDE that offers an action of another product reads the id here instead of keeping it. An editor
// without a key has no such action; `vscodeArguments` are what the VS Code command is called with.
Json actions() {
    const std::string plugin { "community.mcpp.mcppls." };
    return Json::array({
        Json { { "id", "restart" }, { "title", "Restart the language server" },
               { "vscode", "mcppls.restartServer" }, { "jetbrains", plugin + "RestartServer" }, { "nvim", "McpplsRestart" } },
        Json { { "id", "reload" }, { "title", "Read the build description again" },
               { "vscode", "mcppls.reloadBuildDescription" }, { "nvim", "McpplsReload" } },
        Json { { "id", "restartEngine" }, { "title", "Restart the engine" }, { "vscode", "mcppls.restartClangd" } },
        Json { { "id", "resetCache" }, { "title", "Reset the workspace cache" },
               { "vscode", "mcppls.resetWorkspaceCache" }, { "jetbrains", plugin + "ResetWorkspaceCache" }, { "nvim", "McpplsResetCache" } },
        Json { { "id", "showStatus" }, { "title", "Show the status" }, { "nvim", "McpplsStatus" } },
        Json { { "id", "showLog" }, { "title", "Show the log" }, { "vscode", "mcppls.showLogs" } },
        Json { { "id", "openLogDirectory" }, { "title", "Open the log directory" },
               { "vscode", "mcppls.revealCacheDirectory" }, { "vscodeArguments", Json::array({ "logs" }) } },
        Json { { "id", "openCacheDirectory" }, { "title", "Open the workspace cache directory" }, { "vscode", "mcppls.revealCacheDirectory" } },
        Json { { "id", "collectReport" }, { "title", "Collect a report" }, { "vscode", "mcppls.collectReport" } },
        Json { { "id", "exportDiagnostics" }, { "title", "Export a diagnostic bundle" }, { "vscode", "mcppls.exportDiagnosticBundle" } },
        Json { { "id", "selectContext" }, { "title", "Select the analysis context" }, { "vscode", "mcppls.selectContext" } },
        Json { { "id", "moduleGraph" }, { "title", "Show the module graph" }, { "vscode", "mcppls.showModuleGraph" } },
        Json { { "id", "runBuildTool" }, { "title", "Run the build tool in a terminal" }, { "vscode", "mcppls.runBuildToolInTerminal" } },
        Json { { "id", "installTools" }, { "title", "Install the command line tools" }, { "vscode", "mcppls.installCommandLineTools" } },
        Json { { "id", "reviewRun" }, { "title", "Review the workspace's changes" }, { "vscode", "mcppls.review.run" } },
        Json { { "id", "reviewClear" }, { "title", "Clear the review" }, { "vscode", "mcppls.review.clear" } },
        Json { { "id", "turnOffInWorkspace" }, { "title", "Turn off in this workspace" }, { "vscode", "mcppls.turnOffInWorkspace" } },
        Json { { "id", "turnOnInWorkspace" }, { "title", "Turn on in this workspace" }, { "vscode", "mcppls.turnOnInWorkspace" } },
        Json { { "id", "turnOffOtherCppFeatures" }, { "title", "Turn off other C++ features" },
               { "vscode", "mcppls.turnOffOtherCppFeatures" }, { "jetbrains", plugin + "UseMcpplsOnly" } },
        Json { { "id", "restoreOtherCppFeatures" }, { "title", "Restore other C++ features" },
               { "vscode", "mcppls.restoreOtherCppFeatures" }, { "jetbrains", plugin + "RestoreClionEngine" } },
    });
}

// Installing the kit is an action only while a kit is missing, and it names the target.
Json install_kit_action(const std::string& target) {
    return Json { { "id", "installKit" }, { "title", "Install the semantic kit" }, { "vscode", "mcppls.installKit" },
                  { "vscodeArguments", Json::array({ Json { { "target", target } } }) },
                  { "command", std::format("xlings install mcppls-kit-{}", target) } };
}

std::int64_t milliseconds_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::optional<Json> read_json(const std::string& path) {
    const auto text = os::fs::read(path);
    if (!text) return std::nullopt;
    Json document = Json::parse(*text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;
    return document;
}

// The state of one instance in the vocabulary of this command: S3's `starting`, `loading` and
// `preparing` are all `starting` to a reader that only asks whether the server answers yet.
std::string coarse(std::string_view state) {
    if (state == "ready") return "ready";
    if (state == "degraded" || state == "error") return "degraded";
    return "starting";
}

int rank(std::string_view state) {
    if (state == "ready") return 3;
    if (state == "starting") return 2;
    if (state == "degraded") return 1;
    return 0;
}

struct Instance {
    Json instance;   // instance.json
    Json status = Json::object();   // status.json, or empty when the instance has not stated one yet
    bool alive { false };
};

std::vector<Instance> instances_of(const std::string& workspaceDirectory) {
    std::vector<Instance> out;
    std::vector<std::string> directories { workspaceDirectory };
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(base::join_path(workspaceDirectory, "instances"), ignored)) {
        if (entry.is_directory(ignored)) directories.push_back(base::spelled(entry.path()));
    }
    for (const auto& directory : directories) {
        auto instance = read_json(base::join_path(directory, "instance.json"));
        if (!instance) continue;
        Instance found;
        const std::int64_t at { instance->value("at", std::int64_t { 0 }) };
        const std::int64_t pid { instance->value("pid", std::int64_t { 0 }) };
        const bool fresh { milliseconds_now() - at < std::chrono::milliseconds { orchestrator::LEASE_EXPIRY }.count() };
        found.alive = fresh && !orchestrator::lease_owner_gone(pid, instance->value("started", std::string {}));
        found.instance = std::move(*instance);
        if (auto status = read_json(base::join_path(directory, "status.json"))) found.status = std::move(*status);
        out.push_back(std::move(found));
    }
    return out;
}

} // namespace

nlohmann::json status_document(std::string_view rootText, std::string_view cacheDirectory) {
    const std::string root { rootText };
    const std::string workspaceDirectory { base::join_path(cacheDirectory.empty() ? base::cache_directory() : std::string { cacheDirectory },
                                                           base::join_path("workspaces", project::workspace_key(root))) };
    const auto payload = engine::resolve_payload(engine::PayloadRequest {});
    Json document {
        { "schema", 1 },
        { "server", Json { { "version", std::string { base::VERSION } }, { "platform", std::string { base::payload_platform() } },
                           { "payload", payload.directory } } },
        { "capabilities", Json::array() },
        { "actions", actions() },
    };
    for (const auto name : CAPABILITIES) document["capabilities"].push_back(std::string { name });

    Json workspace { { "root", root }, { "state", "none" } };
    // The kit, as this mcppls resolves it for this host without a server (S4 section 5).
    {
        // A kit that stands in for the target's own (the openkal kit, another host's) names the
        // published kit that would serve it: that one is missing, and installing it is the action.
        const auto choice = engine::kit_for_target(payload, std::string {});
        if (!choice.missingTarget.empty()) {
            workspace["kit"] = Json { { "name", std::format("mcppls-kit-{}", choice.missingTarget) }, { "state", "missing" },
                                      { "install", std::format("xlings install mcppls-kit-{}", choice.missingTarget) } };
            document["actions"].push_back(install_kit_action(choice.missingTarget));
        }
        else if (choice.kit) workspace["kit"] = Json { { "name", choice.kit->name }, { "state", "ok" } };
        else workspace["kit"] = Json { { "name", "" }, { "state", "missing" } };
    }
    Json instances = Json::array();
    const Instance* best = nullptr;
    bool anyRecorded { false };
    const auto recorded = instances_of(workspaceDirectory);   // `best` points into it after the loop
    for (const auto& found : recorded) {
        anyRecorded = true;
        if (!found.alive) continue;   // a process that is gone left its files; they describe nothing now
        const Json& sent = found.status.is_object() ? found.status.value("status", Json::object()) : Json::object();
        const Json& about = sent.value("about", Json::object());
        Json entry {
            { "pid", found.instance.value("pid", std::int64_t { 0 }) },
            { "state", coarse(sent.value("state", std::string { "starting" })) },
            { "since", about.value("startedAt", std::string {}) },
        };
        if (about.contains("editor")) entry["editor"] = about["editor"].value("name", std::string {});
        instances.push_back(entry);
        if (!best || rank(entry["state"].get<std::string>()) > rank(coarse(best->status.value("status", Json::object()).value("state", std::string {}))))
            best = &found;
    }
    if (best) {
        const Json sent = best->status.value("status", Json::object());
        const Json about = sent.value("about", Json::object());
        workspace["state"] = coarse(sent.value("state", std::string { "starting" }));
        workspace["since"] = about.value("startedAt", std::string {});
        if (sent.contains("engine")) {
            Json engine = sent["engine"];
            if (about.contains("engine") && about["engine"].contains("restarts")) engine["restarts"] = about["engine"]["restarts"];
            workspace["engine"] = std::move(engine);
        }
        // The kit the server reads with: the published kit, or the openkal kit standing in for it, which
        // leaves the published one missing and its installation an action.
        if (about.contains("kit")) workspace["kit"]["inUse"] = about["kit"];
        if (best->status.contains("mcpp")) workspace["mcpp"] = best->status["mcpp"];
        Json problems = Json::array();
        for (const auto& issue : sent.value("issues", Json::array()))
            if (issue.is_object()) problems.push_back(Json { { "code", issue.value("code", std::string {}) }, { "message", issue.value("message", std::string {}) } });
        workspace["problems"] = std::move(problems);
    } else if (anyRecorded) {
        workspace["state"] = "stopped";
    }
    document["workspace"] = std::move(workspace);
    document["instances"] = std::move(instances);
    return document;
}

cmdline::App status_command(bool& handled, int& status) {
    cmdline::App command { "status" };
    (void)command.description("The state of the servers serving a workspace, without starting an engine (S3 section 8)");
    (void)command.option("root").takes_value().help("Workspace root (default: the current directory)");
    (void)command.option("format").takes_value().help("text (default) | json");
    (void)command.action([&handled, &status](const cmdline::ParsedArgs& args) {
        handled = true;
        const std::string root { base::normalize_path(args.value("root") ? absolute(*args.value("root")) : base::current_directory()) };
        const Json document = status_document(root);
        if (args.value("format").value_or("text") == "json") {
            std::println("{}", document.dump(2));
        } else {
            const Json& workspace = document["workspace"];
            std::println("mcppls {} ({})", document["server"]["version"].get<std::string>(), document["server"]["platform"].get<std::string>());
            std::println("workspace {}: {}", workspace["root"].get<std::string>(), workspace["state"].get<std::string>());
            if (workspace.contains("engine")) std::println("  engine {} {}", workspace["engine"].value("name", std::string {}), workspace["engine"].value("version", std::string {}));
            if (workspace.contains("kit")) std::println("  kit {} ({})", workspace["kit"].value("name", std::string {}), workspace["kit"].value("state", std::string {}));
            if (workspace.contains("mcpp")) std::println("  mcpp {}", workspace["mcpp"].value("version", std::string {}));
            for (const auto& instance : document["instances"])
                std::println("  instance {} {}{}", instance.value("pid", std::int64_t { 0 }), instance.value("state", std::string {}),
                             instance.contains("editor") ? std::format(" ({})", instance["editor"].get<std::string>()) : std::string {});
        }
        status = 0;
    });
    return command;
}

} // namespace mcppls::cli
