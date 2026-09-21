// Request routing and result merging (overall design 5.2): which engines a client request goes to,
// by the methods each engine declares and whether it claims the request, and how the results of
// engines that merge are combined.
export module mcppls.orchestrator.routing;

import std;
import nlohmann.json;
import mcppls.engine;

export namespace mcppls::orchestrator {

using Json = nlohmann::json;

struct Selection {
    std::vector<engine::Engine*> answerers;   // asked one after another until one gives a result
    std::vector<engine::Engine*> mergers;     // asked together; their results are merged
};

// The engines that declared `request.method` (or, for an engine that did not, EVERY_METHOD) and
// claim it: merging engines together; otherwise answering engines by priority, then fallbacks.
Selection select_engines(std::span<engine::Engine* const> engines, const engine::RequestView& request);

// The id of the engine whose results are the module index's in a merge (mcppls's own engine).
inline constexpr std::string_view MODULE_ENGINE_ID { "mcppls" };

// Combines the results of the engines that merged a request: the module engine's with the others'.
Json merge_results(std::string_view method, std::span<const std::pair<std::string, Json>> results);

Json merge_document_symbols(const Json& engineResult, const Json& moduleSymbols);
Json merge_workspace_symbols(const Json& engineResult, const Json& moduleSymbols);
Json merge_diagnostics(const Json& engineDiagnostics, const Json& moduleDiagnostics, std::string_view engineSourceLabel);
// The core engine's capabilities with this server's additions.
Json merge_capabilities(const Json& engineCapabilities);
// Whether the client advertised experimental.cxxModules.<feature>.
bool client_supports(const Json& clientCapabilities, std::string_view feature);

} // namespace mcppls::orchestrator
