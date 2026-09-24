// mcppls's own engine (overall design 5.3): it answers in process from the workspace's module index.
// Stage N1: module-name navigation and hover, import completion, and module symbols merged into the
// outline and workspace symbols. Its module diagnostics are merged by the workspace itself.
export module mcppls.engine.native;

import std;
import mcppls.engine;
import mcppls.engine.native.index;

export namespace mcppls::engine::native {

inline constexpr std::string_view ENGINE_ID { "mcppls" };

// initializationOptions.semanticTokens (design doc 2026-09-25 K/§7, contract T0).
struct TokenOptions {
    bool modules { true };       // native module-syntax tokens (export/module/import, module names) at all
    bool moduleType { false };   // the client knows the custom `module` type and `partition` modifier
};

// `index` is the workspace's, and outlives the engine.
std::unique_ptr<Engine> make_engine(const index::ModuleIndex& index, TokenOptions tokenOptions = {});

} // namespace mcppls::engine::native
