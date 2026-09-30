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

// R-7 (plan 2026-09-30): the identifiers of `text` that begin like the word typed before `position`, nearest first
// and at most `limit`, as completion items -- what mcppls has to offer while the core engine has not answered a
// completion in its budget. None before anything is typed, and none after `.`, `->` or `::`, where only the core
// engine knows what may follow; comments and string and character literals are not read.
Json document_words(std::string_view text, base::Position position, std::size_t limit = 50);

// M-2 (plan 0.0.8): a completion result with nothing in it -- null, an empty array, or a list without items.
bool is_empty(const Json& result);

// M-2: the text of a line before the cursor is inside an import directive (`[export] import` and a blank, and
// whatever follows), where only module names belong -- never the file's words.
bool in_import_directive(std::string_view linePrefix);

// R-7: the list that goes out when the core engine has not answered a completion in its budget: `wordItems`
// (document_words) as an incomplete list, so the client asks again as the person types on. The keywords are
// merged in by the caller, as into any answer (merge).
Json without_engine(const Json& wordItems);

// C-2 (plan 0.0.8 part 2): the word a completion is asked in -- its line, the text of that line before the word -- and
// what of the word has been typed up to the cursor.
struct WordKey {
    int line { 0 };
    std::string before;
    std::string typed;
    bool operator==(const WordKey&) const = default;
};
// nullopt when `position` is not in `text`.
std::optional<WordKey> word_key(std::string_view text, base::Position position);
// `later` is the same word as `earlier` typed on: what an answer for `earlier` offers still covers it, since the client
// filters it by what was typed since. A word typed back (a backspace), or another word, is not.
bool typed_on(const WordKey& earlier, const WordKey& later);

// C-2: a completion answered for an earlier position in the same word, for `position`: each item's edit that replaced
// the word up to the old cursor replaces it up to this one (a client drops an item whose range does not contain the
// position it asked at). Items without an edit are kept as they are; an edit on another line or starting after
// `position` drops its item. The result is a CompletionList, with the engine's `isIncomplete`.
Json retarget(const Json& result, base::Position position);

} // namespace mcppls::orchestrator::completion
