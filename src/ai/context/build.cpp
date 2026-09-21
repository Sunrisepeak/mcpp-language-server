module mcppls.ai.context.build;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.spec.database;
import mcppls.spec.query;
import mcppls.project.detect;
import mcppls.project.model;
import mcppls.project.scan;
import mcppls.normalize.plan;
import mcppls.engine;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;

namespace mcppls::ai::context {

namespace {

using Json = nlohmann::json;

std::string macro_text(const spec::Macro& macro) {
    if (macro.undefine) return "-" + macro.name;
    return macro.value ? std::format("{}={}", macro.name, *macro.value) : macro.name;
}

} // namespace

query::Outcome<BuildContext> build_context(query::View& view, std::string_view file, query::Clock::time_point deadline) {
    view.refresh();
    const std::string path { view.path_of(file) };
    if (!view.kernel().text(path)) return std::unexpected { query::invalid_arguments(std::format("no such file: {}", file)) };
    (void)view.settle(deadline);
    auto& workspace = view.kernel().workspace();

    BuildContext context;
    context.file = view.display(path);
    context.module = view.module_of(path);
    const auto model = workspace.project_model();
    const Json status = view.kernel().status();
    const Json profile = status.is_object() ? status.value("profile", Json::object()) : Json::object();
    context.semanticSource = profile.value("kind", std::string {});
    context.compiler = profile.value("compiler", std::string {});
    context.stdlib = profile.value("stdlib", std::string {});
    context.target = profile.value("target", std::string {});
    if (const auto core = workspace.core_engine_status()) context.engine = std::format("{} {}", core->name, core->version);
    else context.engine = "none";
    const Json contexts = workspace.contexts();
    context.contextSet = contexts.value("current", std::string { "default" });

    const std::string key { base::path_key(path) };
    if (model) {
        context.projectSource = std::string { project::to_string(model->source) };
        for (const auto& set : model->database.sets) {
            for (const auto& unit : set.units) {
                if (base::path_key(spec::absolute_source(unit)) != key) continue;
                context.inModel = true;
                const spec::Role role { unit.role.value_or(spec::Role::unknown) };
                context.sets.push_back(SetMembership { set.name, set.kind, std::string { spec::to_string(role) } });
                if (context.sets.size() > 1) continue;   // the first set's facts describe the file
                const auto facts = model->facts.find(set.toolchain);
                const spec::Toolchain* toolchain { facts != model->facts.end() ? &facts->second.toolchain : spec::find_toolchain(model->database, set.toolchain) };
                if (toolchain != nullptr) {
                    context.toolchainFamily = std::string { spec::to_string(toolchain->family) };
                    context.toolchainVersion = toolchain->version;
                    if (!toolchain->target.empty()) context.target = toolchain->target;
                    if (toolchain->stdlib) context.stdlib = std::format("{} {}", toolchain->stdlib->name, toolchain->stdlib->version);
                }
                const std::optional<spec::SemanticOptions>& options { unit.options ? unit.options : set.options };
                if (options) {
                    context.languageStandard = options->languageStandard.value_or("");
                    for (const auto& macro : options->macros) context.macros.push_back(macro_text(macro));
                    for (const auto& directory : options->includeDirectories.user) context.includeDirectories.push_back(view.display(directory));
                    for (const auto& directory : options->includeDirectories.quote) context.includeDirectories.push_back(view.display(directory));
                }
            }
        }
        for (const auto& issue : model->issues) context.issues.push_back(BuildIssue { issue.code, issue.message });
    }
    const auto& plan = workspace.engine_plan();
    context.excludedFromEngine = std::ranges::any_of(plan.excludedFiles, [&](const std::string& excluded) { return base::path_key(excluded) == key; });
    for (const auto& issue : plan.issues) {
        if (base::path_key(issue.file) == key) context.issues.push_back(BuildIssue { issue.code, issue.message });
    }
    if (!context.inModel) {
        context.issues.push_back(BuildIssue { "not-in-model", "no set of the project model builds this file; it is read with the workspace's default profile" });
    }
    context.snapshot = view.snapshot();
    return context;
}

Json to_json(const BuildContext& context) {
    Json sets = Json::array();
    for (const auto& set : context.sets) sets.push_back(Json { { "name", set.name }, { "kind", set.kind }, { "role", set.role } });
    Json issues = Json::array();
    for (const auto& issue : context.issues) issues.push_back(Json { { "code", issue.code }, { "message", issue.message } });
    Json value { { "snapshot", spec::to_json(context.snapshot) },
                 { "file", context.file },
                 { "inModel", context.inModel },
                 { "sets", std::move(sets) },
                 { "contextSet", context.contextSet },
                 { "projectSource", context.projectSource },
                 { "semanticSource", context.semanticSource },
                 { "target", context.target },
                 { "stdlib", context.stdlib },
                 { "macros", context.macros },
                 { "includeDirectories", context.includeDirectories },
                 { "excludedFromEngine", context.excludedFromEngine },
                 { "engine", context.engine },
                 { "issues", std::move(issues) } };
    if (!context.module.empty()) value["module"] = context.module;
    if (!context.compiler.empty()) value["compiler"] = context.compiler;
    if (!context.toolchainFamily.empty()) value["toolchain"] = Json { { "family", context.toolchainFamily }, { "version", context.toolchainVersion } };
    if (!context.languageStandard.empty()) value["languageStandard"] = context.languageStandard;
    return value;
}

} // namespace mcppls::ai::context
