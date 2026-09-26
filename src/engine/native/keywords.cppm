// Module-syntax keyword completion (fix plan 2026-09-26 F15, decision D5): `import`,
// `export import`, `module;`, `export module`, `module` and `module :private;` where each can
// begin a declaration, from the text alone. clangd offers some of them, but not the combined
// forms, and nothing at all while it is stuck on the file; these come from mcppls whatever
// clangd is doing, and are merged with what clangd answers.
export module mcppls.engine.native.keywords;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.project.scan;

export namespace mcppls::index {

struct KeywordOptions {
    // For a client that runs VS Code's commands: accepting `import` or `export import` opens the
    // module list at once (editor.action.triggerSuggest), without waiting for a space.
    bool suggestModulesAfterImport { false };
};

// The keyword items for `position` in `text`: at the start of a line at global scope (outside any
// braces, comments and literals), with a partial word, or `export` and a partial word, before the
// cursor. `scan` is the file's scan, for which declarations the file already has. An empty array
// when the position is such a place and no keyword fits what was typed; null when it is not.
nlohmann::json keyword_completion(std::string_view text, base::Position position, const project::ScanResult* scan, KeywordOptions options = {});

} // namespace mcppls::index
