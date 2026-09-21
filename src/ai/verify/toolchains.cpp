module mcppls.ai.verify.toolchains;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.spec.query;
import mcppls.project.detect;
import mcppls.project.model;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;

namespace mcppls::ai::verify {

namespace {

using Json = nlohmann::json;
using query::Clock;

// What a build writes, or what is not the project: never copied, never compared.
constexpr std::array<std::string_view, 4> SKIPPED { "target", ".git", ".cache", ".mcpp" };

bool skipped(std::string_view name) { return std::ranges::find(SKIPPED, name) != SKIPPED.end(); }

// Makes `to` hold what `from` holds, writing only what differs, so each toolchain's build stays incremental.
void sync_tree(const std::string& from, const std::string& to) {
    (void)platform::fs::create_directories(to);
    std::set<std::string> present;
    for (const auto& entry : platform::fs::list_directory(from)) {
        const std::string name { base::file_name(entry) };
        if (skipped(name)) continue;
        present.insert(name);
        const std::string target { base::join_path(to, name) };
        if (platform::fs::is_directory(entry)) {
            sync_tree(entry, target);
            continue;
        }
        auto content = platform::fs::read_file(entry);
        if (!content) continue;
        if (platform::fs::read_file(target).value_or(std::string {}) != *content || !platform::fs::exists(target)) (void)platform::fs::write_file(target, *content);
    }
    for (const auto& entry : platform::fs::list_directory(to)) {
        const std::string name { base::file_name(entry) };
        if (!skipped(name) && !present.contains(name)) platform::fs::remove_all(entry);
    }
}

std::string sanitized(std::string_view spec) {
    std::string name { spec };
    for (char& c : name) {
        if (!base::is_identifier_char(c) && c != '.' && c != '-') c = '_';
    }
    return name;
}

std::optional<int> number(std::string_view text) {
    if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    return std::stoi(std::string { text });
}

} // namespace

std::vector<CompilerDiagnostic> parse_compiler_output(std::string_view output, std::string_view buildRoot, std::string_view root,
                                                      const std::function<std::string(std::string_view path)>& textOf) {
    std::vector<CompilerDiagnostic> diagnostics;
    std::set<std::tuple<std::string, int, std::string>> seen;
    for (auto raw : base::split_lines(output)) {
        std::string_view line { base::trim(raw) };
        std::string path;
        int lineNumber { 0 };
        int column { 1 };
        std::string severity;
        std::string message;
        bool matched { false };
        for (const std::string_view marker : { ": fatal error: ", ": error: ", ": warning: " }) {
            const std::size_t at { line.find(marker) };
            if (at == std::string_view::npos) continue;
            // path:line:column before the marker; a Windows drive's colon stays in the path.
            std::string_view head { line.substr(0, at) };
            const std::size_t second { head.rfind(':') };
            if (second == std::string_view::npos) continue;
            const std::size_t first { head.rfind(':', second == 0 ? 0 : second - 1) };
            if (first == std::string_view::npos) continue;
            const auto parsedLine = number(head.substr(first + 1, second - first - 1));
            const auto parsedColumn = number(head.substr(second + 1));
            if (!parsedLine || !parsedColumn) continue;
            path = std::string { head.substr(0, first) };
            lineNumber = *parsedLine;
            column = *parsedColumn;
            severity = marker == ": warning: " ? "warning" : "error";
            message = std::string { line.substr(at + marker.size()) };
            matched = true;
            break;
        }
        if (!matched) {
            // MSVC: file(line,column): error C1234: message
            for (const std::string_view marker : { "): fatal error ", "): error ", "): warning " }) {
                const std::size_t at { line.find(marker) };
                if (at == std::string_view::npos) continue;
                const std::size_t open { line.rfind('(', at) };
                if (open == std::string_view::npos) continue;
                const std::string_view position { line.substr(open + 1, at - open - 1) };
                const std::size_t comma { position.find(',') };
                const auto parsedLine = number(position.substr(0, comma));
                if (!parsedLine) continue;
                path = std::string { line.substr(0, open) };
                lineNumber = *parsedLine;
                column = comma == std::string_view::npos ? 1 : number(position.substr(comma + 1)).value_or(1);
                severity = marker == "): warning " ? "warning" : "error";
                std::string_view rest { line.substr(at + marker.size()) };
                if (const std::size_t colon { rest.find(": ") }; colon != std::string_view::npos) rest = rest.substr(colon + 2);
                message = std::string { rest };
                matched = true;
                break;
            }
        }
        if (!matched) continue;
        const std::string absolute { base::is_absolute_path(path) ? base::normalize_path(path) : base::normalize_path(base::join_path(buildRoot, path)) };
        const auto relative = base::relative_path(absolute, buildRoot);
        if (!relative || !base::is_within(absolute, buildRoot)) continue;   // a toolchain's own header, not the project's
        std::string display { *relative };
        std::ranges::replace(display, '\\', '/');
        if (display.starts_with("target/")) continue;
        if (!seen.emplace(display, lineNumber, message).second) continue;
        const std::string workspacePath { base::join_path(root, *relative) };
        CompilerDiagnostic diagnostic;
        diagnostic.severity = std::move(severity);
        diagnostic.message = std::move(message);
        const std::string text { textOf(workspacePath) };
        diagnostic.location = spec::Location { display, lineNumber, column, spec::line_of(text, lineNumber - 1) };
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

query::Outcome<ToolchainComparison> compare_toolchains(query::View& view, std::span<const std::string> toolchains, Clock::time_point deadline) {
    auto& workspace = view.kernel().workspace();
    if (!workspace.trusted()) return std::unexpected { query::Failure { "untrusted", "the workspace is not trusted, so no toolchain is run", nullptr } };
    if (toolchains.size() < 2) return std::unexpected { query::invalid_arguments("name at least two toolchains to compare") };
    (void)view.settle(deadline);
    const auto model = workspace.project_model();
    if (!model || model->detected != project::SourceKind::mcpp) {
        return std::unexpected { query::Failure { "unavailable", "cross-toolchain verification builds mcpp projects, and this workspace is not one", nullptr } };
    }
    std::string mcpp { workspace.mcpp_executable() };
    if (mcpp.empty()) mcpp = platform::env::find_executable("mcpp").value_or("");
    if (mcpp.empty()) return std::unexpected { query::Failure { "unavailable", "mcpp is not on PATH", nullptr } };

    ToolchainComparison comparison;
    for (const auto& toolchain : toolchains) {
        ToolchainBuild build;
        build.toolchain = toolchain;
        const std::string copy { base::join_path(workspace.cache_directory(), base::join_path("toolchains", sanitized(toolchain))) };
        sync_tree(view.root(), copy);
        platform::SpawnOptions spawn;
        spawn.program = mcpp;
        spawn.arguments = { "build", "--toolchain", toolchain };
        spawn.workDirectory = copy;
        const auto remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()), std::chrono::milliseconds { 1000 });
        const auto started = Clock::now();
        auto result = platform::toolrun::run({
            .program = spawn.program,
            .arguments = spawn.arguments,
            .workDirectory = spawn.workDirectory,
            .purpose = "review-build",
            .bounds = platform::RunBounds { .hard = remaining },
            .environment = spawn.environment,
        });
        build.seconds = std::chrono::duration<double>(Clock::now() - started).count();
        if (result && !result->timedOut) {
            build.ran = true;
            build.exitCode = result->exitCode;
            build.built = result->exitCode == 0;
            const std::string output { result->output + "\n" + result->error };
            build.diagnostics = parse_compiler_output(output, copy, view.root(), [&](std::string_view path) { return view.text_of(path); });
            if (!build.built && std::ranges::none_of(build.diagnostics, [](const CompilerDiagnostic& d) { return d.severity == "error"; })) {
                build.output = output.size() > 2000 ? output.substr(output.size() - 2000) : output;
            }
        } else {
            build.output = result ? std::string { "the build did not finish in time" } : result.error().message;
        }
        comparison.builds.push_back(std::move(build));
    }
    // An error of a toolchain whose build failed, where another toolchain built the project.
    for (const auto& build : comparison.builds) {
        if (!build.ran || build.built) continue;
        std::vector<std::string> others;
        for (const auto& other : comparison.builds) {
            if (other.built) others.push_back(other.toolchain);
        }
        if (others.empty()) continue;
        for (const auto& diagnostic : build.diagnostics) {
            if (diagnostic.severity == "error") comparison.divergences.push_back(Divergence { build.toolchain, others, diagnostic });
        }
    }
    return comparison;
}

Json to_json(const ToolchainComparison& comparison) {
    const auto diagnostic_json = [](const CompilerDiagnostic& diagnostic) {
        return Json { { "severity", diagnostic.severity }, { "message", diagnostic.message }, { "location", spec::to_json(diagnostic.location) } };
    };
    Json builds = Json::array();
    for (const auto& build : comparison.builds) {
        Json diagnostics = Json::array();
        for (const auto& diagnostic : build.diagnostics) diagnostics.push_back(diagnostic_json(diagnostic));
        Json value { { "toolchain", build.toolchain }, { "built", build.built }, { "ran", build.ran }, { "exitCode", build.exitCode },
                     { "seconds", build.seconds }, { "diagnostics", std::move(diagnostics) } };
        if (!build.output.empty()) value["output"] = build.output;
        builds.push_back(std::move(value));
    }
    Json divergences = Json::array();
    for (const auto& divergence : comparison.divergences) {
        divergences.push_back(Json { { "toolchain", divergence.toolchain }, { "others", divergence.others }, { "diagnostic", diagnostic_json(divergence.diagnostic) } });
    }
    return Json { { "builds", std::move(builds) }, { "divergences", std::move(divergences) } };
}

} // namespace mcppls::ai::verify
