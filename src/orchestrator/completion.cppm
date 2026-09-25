// Completion routing (fix plan 2026-09-26 F9, F15, decisions D4, D5): a space after `import`
// opens the module list, and module-syntax keywords come from mcppls whatever the core engine is
// doing. Pure functions: the workspace calls them on its routing path.
//
// A space is a trigger character only where it cannot cost anything (D4). The editor drops a
// space-triggered request outside an import line before it is sent (VS Code's middleware); the
// server answers one that arrives anyway at once, empty, from the line's own text, without asking
// an engine; the module names are cached by the index; and the space is advertised only to
// clients that do the first step, or that ask for it.
export module mcppls.orchestrator.completion;

import std;
import nlohmann.json;
import mcppls.base.text;

export namespace mcppls::orchestrator::completion {

using Json = nlohmann::json;

// `^\s*(export\s+)?import\s$`: the text of a line before the cursor is an import directive's
// keyword and exactly one blank after it, with nothing typed after that.
bool is_import_line_prefix(std::string_view linePrefix);

// The text of the line `position` is on, before it; nullopt when the position is not in `text`.
std::optional<std::string_view> line_prefix(std::string_view text, base::Position position);

// A completion request the client sent because a space was typed (CompletionTriggerKind.TriggerCharacter).
bool is_space_trigger(const Json& params);

// The client runs this repository's VS Code extension, or one built on the same extension host:
// VS Code and its forks name themselves in clientInfo (Visual Studio Code, Code - OSS, VSCodium,
// Cursor, Windsurf, Trae, Positron). Such a client also runs the extension's middleware.
bool vscode_like(const Json& clientParams);

// D4 layer 4: whether the server advertises the space as a trigger character to this client:
// initializationOptions.completion.triggerOnSpace when the client gave it, else vscode_like.
bool space_trigger_wanted(const Json& clientParams);

// Adds " " to capabilities.completionProvider.triggerCharacters.
void add_space_trigger(Json& capabilities);

// The answer to a space-triggered request that is not on an import line: an empty, complete list.
Json empty_list();

// F15: the core engine's completion result (null, CompletionItem[] or CompletionList) with the
// module-syntax keyword items added. A keyword whose label the engine already gave is not added
// twice. `isIncomplete` is the engine's; null stays null when there are no keywords.
Json merge(const Json& engineResult, const Json& keywordItems);

// Only keywords, as an incomplete list: the core engine did not answer in time, and the client
// asks again as the person types on.
Json keywords_only(const Json& keywordItems);

} // namespace mcppls::orchestrator::completion
