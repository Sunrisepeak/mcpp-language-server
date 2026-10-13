// The answer layer (answer-layer design, 2026-10-13): answers to the requests a person waits for -- completion,
// signature help, hover, the jumps -- kept by what they depend on, so that the same kind of request seen once is
// answered at once afterwards, from whichever file, in whichever session; the core engine's own answer replaces it
// when it comes.
//
// Three keys, from the most to the least specific:
//   K1  the position: document, enclosing declaration, the kind of place, the receiver or qualifier's text;
//   K2  the meaning: the type a receiver has (learned from the engine), with the visibility fingerprint;
//   K3  the name: the qualifier spelled out (`std::`, `fs::` as `std::filesystem::`), with the visibility fingerprint.
// Whether a kept answer is still the engine's is decided by generations (mcppls counts what changed) and fingerprints;
// one that may not be is still given, marked uncertain where a person or an agent can tell.
//
// Pure: no workspace state. The workspace hands it texts, generations and the engine's answers, and asks it what to
// answer; it keeps the entries and writes them where it is told.
export module mcppls.orchestrator.answer;

import std;
import nlohmann.json;
import mcppls.base.text;

export namespace mcppls::orchestrator::answer {

using Json = nlohmann::json;

// ---- what is asked ------------------------------------------------------------------------------------------------

enum class Kind { completion, signature_help, hover, definition, declaration, type_definition, implementation };
std::optional<Kind> kind_of(std::string_view lspMethod);
std::string_view name_of(Kind kind);
// The jumps: answered from a kept answer only when it is certain (design 5.4).
bool is_jump(Kind kind);

// Where the cursor is, read from the text alone (design 4.1).
struct Context {
    enum class Place { member, arrow, qualified, unqualified, call, other };
    Place place { Place::other };
    std::string receiver;          // member, arrow: the receiver's text; qualified: the qualifier; call: the callee
    std::string word;              // the identifier at the position (hover, jumps) or typed before it (completion)
    std::size_t wordStart { 0 };   // byte offsets of the word
    std::size_t wordEnd { 0 };
    std::string enclosing;         // the innermost enclosing declaration, "name@line"; empty at file scope
    std::uint64_t localHash { 0 }; // the enclosing body's text up to the word
    int argument { 0 };            // signature help: which argument the cursor is in
};
std::string_view name_of(Context::Place place);
std::optional<Context> context_at(std::string_view text, base::Position position, Kind kind);

// `fs::path` as `std::filesystem::path` when the text says `namespace fs = std::filesystem;`.
std::string expand_aliases(std::string_view text, std::string_view qualifier);
// The type spelled where `name` is declared before `before` in `text`: `ordered_json` for `ordered_json name = ...`,
// `std::shared_ptr<spdlog::logger>` for a member `std::shared_ptr<spdlog::logger> name;`. nullopt for `auto` or none.
std::optional<std::string> declared_type(std::string_view text, std::size_t before, std::string_view name);

// ---- keys -----------------------------------------------------------------------------------------------------------

std::string k1(std::string_view uri, Kind kind, const Context& context);
// nullopt where the place has no name of its own to share (member access, unqualified completion).
std::optional<std::string> k3(Kind kind, const Context& context, std::string_view expandedQualifier);
std::string k2(Kind kind, Context::Place place, std::string_view typeKey);
// A type as learned from the engine's typeDefinition: its name and where it is defined.
std::string type_key(std::string_view name, std::string_view path, int line);

// ---- what is kept ---------------------------------------------------------------------------------------------------

enum class Source { engine, k1, k2, k3, outline, native };
enum class Certainty { authoritative, certain, uncertain };
std::string_view name_of(Source source);
std::string_view name_of(Certainty certainty);

struct Entry {
    Kind kind { Kind::completion };
    Json result;                         // the engine's answer, trimmed to what is shown (trim)
    std::string fingerprint;             // the visibility fingerprint of the file it was asked in
    std::uint64_t others { 0 };          // changes of other files when it was kept (Generations::others)
    std::uint64_t localHash { 0 };       // the context's local hash then
    std::string provider;                // the file that defines what it is about, when known (K2: the type's)
    int providerLine { -1 };             // where in it (0-based), when known
    std::uint64_t providerGeneration { 0 };
    std::string asked;                   // the document it was asked in
    std::string word;                    // a completion's: the word typed when it was asked (the engine filtered by it)
    bool complete { false };             // a completion's: the engine said the list was complete
    std::int64_t keptAt { 0 };           // system clock, ms
    std::uint32_t hits { 0 };
};

// What to keep of an engine answer: what is shown and inserted; nullopt when there is nothing to keep (empty).
std::optional<Json> trim(Kind kind, const Json& result);

// Kept entries, least recently used out first, within a byte budget.
class Store {
public:
    explicit Store(std::size_t maxBytes = 32u << 20);
    const Entry* find(const std::string& key);
    void put(const std::string& key, Entry entry);
    void erase(const std::string& key);
    std::size_t size() const { return entries_.size(); }
    std::size_t bytes() const { return bytes_; }
    bool contains(const std::string& key) const { return entries_.contains(key); }
    std::uint64_t changes() const { return changes_; }

    // The persisted form: a version, then the entries (K3 only, when `onlyNames`), most recently used last.
    Json to_json(bool onlyNames = false) const;
    // Entries of a persisted form; false when its version is not this one's. Entries already here are kept.
    bool adopt(const Json& persisted);
    static constexpr int FORMAT { 1 };

private:
    struct Slot {
        Entry entry;
        std::size_t bytes { 0 };
        std::list<std::string>::iterator use;
    };
    void evict_();
    std::size_t maxBytes_;
    std::size_t bytes_ { 0 };
    std::uint64_t changes_ { 0 };
    std::list<std::string> order_;   // least recently used first
    std::unordered_map<std::string, Slot> entries_;
};

// ---- certainty (design 4.3) -------------------------------------------------------------------------------------------

struct Now {
    std::string fingerprint;                                     // of the file asked in
    std::uint64_t others { 0 };                                  // changes of files other than the one asked in
    std::uint64_t localHash { 0 };
    std::function<std::uint64_t(std::string_view path)> generation;   // of one file
};
Certainty certainty_of(const Entry& entry, Source source, const Now& now);
// Whether the file that defines what the entry is about changed since: the one uncertainty a person is shown.
bool provider_changed(const Entry& entry, const Now& now);

// ---- composing an answer from a kept one (design 4.6, 5) ------------------------------------------------------------

enum class Marks { subtle, off, verbose };
std::optional<Marks> marks_of(std::string_view setting);

struct Composed {
    Json result;
    std::size_t doubted { 0 };   // items marked as in doubt
};
// A completion list for `context` at the cursor: filtered by the word typed, each item's edit placed on the word,
// incomplete (the client asks again as the person types on, and gets the engine's answer when it has come).
// `members`, when given, is the outline of the type's class as its file is now (design 4.7); without it, a type whose
// file changed (`providerChanged`) has every item marked as in doubt.
Composed compose_completion(const Entry& entry, Source source, Certainty certainty, const Context& context, std::string_view text,
                            base::Position cursor, Marks marks, const std::optional<std::vector<std::string>>& members, bool providerChanged);
Composed compose_hover(const Entry& entry, Source source, Certainty certainty, const Context& context, std::string_view text, Marks marks);
Composed compose_signature(const Entry& entry, const Context& context);
// A jump's locations, when the entry is certain; a moved declaration found once in its file's text otherwise
// (`relocated`), or nothing.
std::optional<Json> compose_jump(const Entry& entry, Certainty certainty, const std::optional<Json>& relocated);

// The items of `served` the engine's `answer` does not have, and those it has that `served` lacked: what the person saw
// differed by. Completion only.
std::pair<std::size_t, std::size_t> difference(const Json& served, const Json& answer);

// ---- where an answer came from (design 4.11, 6) ------------------------------------------------------------------------

struct Provenance {
    Source source { Source::engine };
    Certainty certainty { Certainty::authoritative };
    std::int64_t ageMs { 0 };
    bool providerChanged { false };
    bool enginePending { false };
    std::string note;
};
// For agents: `authoritative` says it all; the rest is detail.
Json to_json(const Provenance& provenance);
std::string note_for(Kind kind, const Provenance& provenance);

} // namespace mcppls::orchestrator::answer
