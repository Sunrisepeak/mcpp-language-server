module mcppls.orchestrator.routing;

import std;
import nlohmann.json;
import mcppls.engine;
import mcppls.lsp.jsonrpc;
import mcppls.orchestrator.tokens;

namespace mcppls::orchestrator {

Selection select_engines(std::span<engine::Engine* const> engines, const engine::RequestView& request) {
    struct Candidate {
        engine::Engine* engine;
        engine::Role role;
        int priority;
    };
    std::vector<Candidate> candidates;
    for (engine::Engine* candidate : engines) {
        if (candidate == nullptr) continue;
        std::optional<engine::MethodCapability> chosen;
        for (const auto& capability : candidate->methods()) {
            if (capability.method == request.method) {
                chosen = capability;
                break;
            }
            if (capability.method == engine::EVERY_METHOD && !chosen) chosen = capability;
        }
        if (!chosen || !candidate->claims(request)) continue;
        candidates.push_back(Candidate { candidate, chosen->role, chosen->priority });
    }
    Selection selection;
    const bool merging { std::ranges::any_of(candidates, [](const Candidate& c) { return c.role == engine::Role::merge; }) };
    std::ranges::stable_sort(candidates, std::greater {}, [](const Candidate& c) { return c.priority; });
    if (merging) {
        for (const auto& candidate : candidates) selection.mergers.push_back(candidate.engine);
        return selection;
    }
    for (const auto& candidate : candidates) {
        if (candidate.role == engine::Role::answer) selection.answerers.push_back(candidate.engine);
    }
    for (const auto& candidate : candidates) {
        if (candidate.role == engine::Role::fallback) selection.answerers.push_back(candidate.engine);
    }
    return selection;
}

Json merge_results(std::string_view method, std::span<const std::pair<std::string, Json>> results) {
    Json moduleResult;
    Json engineResult;
    for (const auto& [engineId, result] : results) {
        if (engineId == MODULE_ENGINE_ID) {
            moduleResult = result;
        } else if (engineResult.is_null()) {
            engineResult = result;
        } else if (engineResult.is_array() && result.is_array()) {
            for (const auto& item : result) engineResult.push_back(item);
        }
    }
    if (method == "textDocument/documentSymbol") return merge_document_symbols(engineResult, moduleResult.is_null() ? Json::array() : moduleResult);
    if (method == "workspace/symbol") return merge_workspace_symbols(engineResult, moduleResult.is_null() ? Json::array() : moduleResult);
    // Semantic tokens (design doc 2026-09-25 K/§7): the core engine's own tokens (already mapped
    // into this server's legend, by the workspace, before they ever reach here -- this function
    // stays pure) win every position they cover; mcppls's module-syntax tokens fill only the gaps.
    if (method == "textDocument/semanticTokens/full" || method == "textDocument/semanticTokens/range") {
        return tokens::merge(engineResult, moduleResult);
    }
    return engineResult.is_null() ? moduleResult : engineResult;
}

Json merge_document_symbols(const Json& engineResult, const Json& moduleSymbols) {
    if (!moduleSymbols.is_array() || moduleSymbols.empty()) return engineResult.is_null() ? Json::array() : engineResult;
    if (!engineResult.is_array() || engineResult.empty()) return moduleSymbols;
    // SymbolInformation[] (flat, with "location") cannot hold DocumentSymbol entries.
    if (engineResult.front().contains("location")) return engineResult;
    Json merged = moduleSymbols;
    for (const auto& symbol : engineResult) merged.push_back(symbol);
    return merged;
}

Json merge_workspace_symbols(const Json& engineResult, const Json& moduleSymbols) {
    Json merged = moduleSymbols.is_array() ? moduleSymbols : Json::array();
    if (engineResult.is_array()) {
        for (const auto& symbol : engineResult) merged.push_back(symbol);
    }
    return merged;
}

Json merge_diagnostics(const Json& engineDiagnostics, const Json& moduleDiagnostics, std::string_view engineSourceLabel) {
    Json merged = Json::array();
    std::set<std::string> moduleRanges;
    if (moduleDiagnostics.is_array()) {
        for (const auto& diagnostic : moduleDiagnostics) {
            moduleRanges.insert(lsp::dump(diagnostic.value("range", Json {})));
            merged.push_back(diagnostic);
        }
    }
    if (engineDiagnostics.is_array()) {
        for (auto diagnostic : engineDiagnostics) {
            // The index already explains an unresolved import at the same place.
            if (moduleRanges.contains(lsp::dump(diagnostic.value("range", Json {})))) continue;
            if (!engineSourceLabel.empty()) {
                const std::string source { diagnostic.value("source", std::string { "clangd" }) };
                diagnostic["source"] = std::format("{} · {}", source, engineSourceLabel);
            }
            merged.push_back(std::move(diagnostic));
        }
    }
    return merged;
}

Json merge_capabilities(const Json& engineCapabilities) {
    Json capabilities = engineCapabilities.is_object() ? engineCapabilities : Json::object();
    if (!capabilities.contains("definitionProvider")) capabilities["definitionProvider"] = true;
    if (!capabilities.contains("hoverProvider")) capabilities["hoverProvider"] = true;
    if (!capabilities.contains("documentSymbolProvider")) capabilities["documentSymbolProvider"] = true;
    if (!capabilities.contains("workspaceSymbolProvider")) capabilities["workspaceSymbolProvider"] = true;
    if (!capabilities.contains("completionProvider")) {
        capabilities["completionProvider"] = Json { { "triggerCharacters", Json::array({ ".", ":" }) } };
    }
    // Semantic tokens (design doc 2026-09-25 K/§7, contract T0): the legend is this server's own,
    // built from whatever the core engine (if any) declared, so a restart or a missing core engine
    // cannot shift an index a client has already seen.
    capabilities["semanticTokensProvider"] = tokens::provider_capability(tokens::build_legend(engineCapabilities));
    if (!capabilities.contains("experimental") || !capabilities["experimental"].is_object()) capabilities["experimental"] = Json::object();
    capabilities["experimental"]["cxxModules"] = Json { { "version", 1 }, { "databaseSpec", ">=0.2 <1" } };
    // usable plan W9.1: without this, a client has no reason to ever send
    // workspace/didChangeWorkspaceFolders, and the session would never learn of an added or
    // removed root.
    if (!capabilities.contains("workspace") || !capabilities["workspace"].is_object()) capabilities["workspace"] = Json::object();
    capabilities["workspace"]["workspaceFolders"] = Json { { "supported", true }, { "changeNotifications", true } };
    // overall design 7.7: the review commands, beside whatever commands the core engine has.
    if (!capabilities.contains("executeCommandProvider") || !capabilities["executeCommandProvider"].is_object()) capabilities["executeCommandProvider"] = Json::object();
    Json& commands = capabilities["executeCommandProvider"]["commands"];
    if (!commands.is_array()) commands = Json::array();
    for (const std::string_view command : { "mcppls.review.run", "mcppls.review.clear", "mcppls.reloadBuildDescription" }) {
        if (std::ranges::find(commands, Json(command)) == commands.end()) commands.push_back(std::string { command });
    }
    return capabilities;
}

bool client_supports(const Json& clientCapabilities, std::string_view feature) {
    const Json* modules { lsp::find_path(clientCapabilities, { "experimental", "cxxModules" }) };
    if (modules == nullptr || !modules->is_object()) return false;
    const auto value = modules->find(feature);
    return value != modules->end() && value->is_boolean() && value->get<bool>();
}

} // namespace mcppls::orchestrator
