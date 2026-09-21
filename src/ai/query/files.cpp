module mcppls.ai.query.files;

import std;
import nlohmann.json;
import mcppls.lsp.jsonrpc;
import mcppls.spec.query;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;

namespace mcppls::ai::query {

namespace {

OutlineEntry entry_of(View& view, std::string_view path, const Json& symbol) {
    OutlineEntry entry;
    entry.name = symbol.value("name", std::string {});
    entry.kind = std::string { symbol_kind_name(symbol.value("kind", 0)) };
    entry.detail = symbol.value("detail", std::string {});
    // DocumentSymbol names its name's range; SymbolInformation only its location.
    const Json range = symbol.contains("selectionRange") ? symbol["selectionRange"] : symbol.value("location", Json::object()).value("range", Json::object());
    entry.line = view.location(path, range).line;
    for (const auto& child : symbol.value("children", Json::array())) entry.children.push_back(entry_of(view, path, child));
    return entry;
}

Json to_json(const OutlineEntry& entry) {
    Json value { { "name", entry.name }, { "kind", entry.kind }, { "line", entry.line } };
    if (!entry.detail.empty()) value["detail"] = entry.detail;
    if (!entry.children.empty()) {
        Json children = Json::array();
        for (const auto& child : entry.children) children.push_back(to_json(child));
        value["children"] = std::move(children);
    }
    return value;
}

std::string code_of(const Json& diagnostic) {
    const auto code = diagnostic.find("code");
    if (code == diagnostic.end()) return {};
    if (code->is_string()) return code->get<std::string>();
    if (code->is_number_integer()) return std::to_string(code->get<std::int64_t>());
    return {};
}

} // namespace

Outcome<Outline> outline_file(View& view, std::string_view file, Clock::time_point deadline) {
    view.refresh();
    const std::string path { view.path_of(file) };
    if (!view.kernel().text(path)) return std::unexpected { invalid_arguments(std::format("no such file: {}", file)) };
    (void)view.settle(deadline);
    view.open(path);
    auto result = view.request("textDocument/documentSymbol", Json { { "textDocument", view.text_document(path) } }, deadline);
    if (!result) return std::unexpected { Failure { "timeout", "the engines did not answer textDocument/documentSymbol in time", nullptr } };
    Outline outline;
    outline.file = view.display(path);
    outline.module = view.module_of(path);
    if (result->is_array()) {
        for (const auto& symbol : *result) outline.symbols.push_back(entry_of(view, path, symbol));
    }
    outline.snapshot = view.snapshot();
    return outline;
}

std::vector<Diagnostic> published_diagnostics(View& view, std::string_view path) {
    std::vector<Diagnostic> diagnostics;
    const auto published = view.kernel().diagnostics(view.kernel().uri_of(path));
    if (!published || !published->is_array()) return diagnostics;
    for (const auto& item : *published) {
        if (!item.is_object()) continue;
        Diagnostic diagnostic;
        diagnostic.severity = spec::severity_from_lsp(item.value("severity", 1));
        diagnostic.message = item.value("message", std::string {});
        diagnostic.code = code_of(item);
        diagnostic.source = item.value("source", std::string {});
        diagnostic.location = view.location(path, item.value("range", Json::object()));
        for (const auto& related : item.value("relatedInformation", Json::array())) {
            auto location = view.location(related.value("location", Json::object()));
            if (!location) continue;
            diagnostic.related.push_back(RelatedInformation { std::move(*location), related.value("message", std::string {}) });
        }
        diagnostics.push_back(std::move(diagnostic));
    }
    std::ranges::stable_sort(diagnostics, {}, [](const Diagnostic& d) { return std::pair { d.location.line, d.location.column }; });
    return diagnostics;
}

std::pair<bool, std::string> diagnostics_fresh(View& view, std::string_view path) {
    auto& workspace = view.kernel().workspace();
    if (!view.has_core_engine()) return { true, {} };
    if (!workspace.core_engine_serves("textDocument/diagnostic", path)) {
        // Left out of the engine's database (an import that cannot resolve): mcppls's own diagnostics are all there is.
        return { true, {} };
    }
    const auto version = view.kernel().version(path);
    const auto published = workspace.core_diagnostics_version(view.kernel().uri_of(path));
    if (!version) return { false, "the file is not open" };
    if (!published) return { false, "the core engine has not published diagnostics for this file yet" };
    // clangd names the version of every document it builds; a list without one is what it sends when a document closes.
    if (*published < *version) return { false, "the core engine's diagnostics are for an earlier version of the file" };
    return { true, {} };
}

std::string semantic_source(View& view) {
    const Json status = view.kernel().status();
    if (!status.is_object()) return {};
    const Json profile = status.value("profile", Json::object());
    const std::string kind { profile.value("kind", std::string {}) };
    if (kind == "semantic-kit") return std::format("semantic kit {}", profile.value("stdlib", std::string {}));
    const std::string compiler { profile.value("compiler", std::string {}) };
    return std::format("build toolchain {}{}", compiler, compiler.empty() ? profile.value("stdlib", std::string {}) : std::string {});
}

Outcome<DiagnosticsReport> file_diagnostics(View& view, std::span<const std::string> files, bool fresh, Clock::time_point deadline) {
    if (files.empty()) return std::unexpected { invalid_arguments("name at least one file") };
    view.refresh();
    std::vector<std::string> paths;
    for (const auto& file : files) {
        const std::string path { view.path_of(file) };
        if (!view.kernel().text(path)) return std::unexpected { invalid_arguments(std::format("no such file: {}", file)) };
        paths.push_back(path);
    }
    (void)view.settle(deadline);
    for (const auto& path : paths) view.open(path);
    if (fresh) {
        const auto remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()), std::chrono::milliseconds { 0 });
        (void)view.kernel().wait_until([&] { return std::ranges::all_of(paths, [&](const std::string& path) { return diagnostics_fresh(view, path).first; }); },
                                       remaining);
    }
    DiagnosticsReport report;
    for (const std::string_view severity : { "error", "warning", "information", "hint" }) report.counts[std::string { severity }] = 0;
    for (const auto& path : paths) {
        FileDiagnostics entry;
        entry.file = view.display(path);
        auto [complete, reason] = diagnostics_fresh(view, path);
        entry.complete = complete;
        entry.reason = std::move(reason);
        entry.diagnostics = published_diagnostics(view, path);
        for (const auto& diagnostic : entry.diagnostics) ++report.counts[std::string { spec::to_string(diagnostic.severity) }];
        report.files.push_back(std::move(entry));
    }
    report.semanticSource = semantic_source(view);
    report.snapshot = view.snapshot();
    return report;
}

Json to_json(const Outline& outline) {
    Json symbols = Json::array();
    for (const auto& entry : outline.symbols) symbols.push_back(to_json(entry));
    Json value { { "snapshot", spec::to_json(outline.snapshot) }, { "file", outline.file }, { "symbols", std::move(symbols) } };
    if (!outline.module.empty()) value["module"] = outline.module;
    return value;
}

Json to_json(const Diagnostic& diagnostic) {
    Json value { { "severity", std::string { spec::to_string(diagnostic.severity) } }, { "message", diagnostic.message }, { "location", spec::to_json(diagnostic.location) } };
    if (!diagnostic.code.empty()) value["code"] = diagnostic.code;
    if (!diagnostic.source.empty()) value["source"] = diagnostic.source;
    if (!diagnostic.related.empty()) {
        Json related = Json::array();
        for (const auto& item : diagnostic.related) related.push_back(Json { { "location", spec::to_json(item.location) }, { "message", item.message } });
        value["related"] = std::move(related);
    }
    return value;
}

Json to_json(const DiagnosticsReport& report) {
    Json files = Json::array();
    for (const auto& entry : report.files) {
        Json diagnostics = Json::array();
        for (const auto& diagnostic : entry.diagnostics) diagnostics.push_back(to_json(diagnostic));
        Json value { { "file", entry.file }, { "complete", entry.complete }, { "diagnostics", std::move(diagnostics) } };
        if (!entry.reason.empty()) value["reason"] = entry.reason;
        files.push_back(std::move(value));
    }
    return Json { { "snapshot", spec::to_json(report.snapshot) }, { "files", std::move(files) }, { "counts", report.counts }, { "semanticSource", report.semanticSource } };
}

} // namespace mcppls::ai::query
