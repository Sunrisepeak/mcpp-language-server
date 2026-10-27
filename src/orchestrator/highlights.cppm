// The semantic tokens of each open document over time (mcpp design 2026-10-28 §11.1). A large file's
// completion needs a preamble and the text; its tokens need the whole file's syntax tree, which the
// compiler builds after the modules it imports and again after each pause in typing. What the compiler
// last answered for a document is kept here and moved with every edit since, so a request is answered at
// once and corrected by `workspace/semanticTokens/refresh` once the compiler has caught up (H1); what it
// answered for a file's saved text is kept on disk, so the next open of the same text, under the same
// command, compiler and modules, is answered from there before the compiler has read a line (H2). Nothing
// here talks to an engine or a client: the workspace does, and asks this store what to answer.
export module mcppls.orchestrator.highlights;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.orchestrator.tokens;

export namespace mcppls::orchestrator::highlights {

using Json = nlohmann::json;
using tokens::Token;

// One edit of a document's text in LSP positions (UTF-16 code units): [start, end) replaced by a text of
// `lines` line breaks whose last line is `lastLength` units long.
struct Edit {
    base::Position start;
    base::Position end;
    int lines { 0 };
    int lastLength { 0 };
};

// The edits a `textDocument/didChange`'s contentChanges make, in order. `before` is the text they apply to;
// it is read only for a change without a range (the whole text again), whose edit is what lies between the
// common beginning and the common end of the two texts.
std::vector<Edit> edits_of(std::string_view before, const Json& contentChanges);

// `tokens` (sorted) after `edit`: a token before it stays, one after it moves with the text, one it touches
// (an insertion at either end included) is dropped -- the compiler says what the edited text is.
void shift(std::vector<Token>& tokens, const Edit& edit);

// `tokens` (sorted) that overlap `range`, for `textDocument/semanticTokens/range`.
std::vector<Token> within(std::span<const Token> tokens, const base::Range& range);

// A digest of tokens, to tell whether the compiler's latest answer is what a client already has.
std::uint64_t digest(std::span<const Token> tokens);

// ---- the persistent cache (H2) ----------------------------------------------------------------------
//
// One file per document path under the workspace's `tokens/` directory, named by a digest of the path: the
// key the tokens were kept under and the tokens, relative-encoded as LEB128 integers. The key is a digest of
// everything the compiler's answer depends on: the text, the command, the versions of mcppls and the
// compiler, and the module interface units the file reads, transitively (a BMI is a function of its unit
// and its command). A read under any other key is a miss: nothing kept for another text, command, compiler
// or module is ever shown.

struct CacheFile {
    std::string key;
    std::vector<Token> tokens;
};

std::string cache_file_name(std::string_view pathKey);
std::string encode_cache(const CacheFile& file);
std::optional<CacheFile> decode_cache(std::string_view bytes);

// Removes the kept files beyond the newest `maxFiles`, or beyond `maxBytes` in all; the bytes freed.
std::uint64_t prune_cache(const std::string& directory, std::size_t maxFiles, std::uint64_t maxBytes);
inline constexpr std::size_t CACHE_FILES { 1000 };
inline constexpr std::uint64_t CACHE_BYTES { std::uint64_t { 64 } << 20 };

struct Kept {
    std::vector<Token> core;             // the compiler's tokens, in the server's legend, moved with each edit since
    bool known { false };                // the compiler gave tokens for this document
    std::int64_t version { 0 };          // the document version `core` stands for now
    bool exact { false };                // the compiler gave `core` for this text, and nothing it reads changed since
    std::optional<std::int64_t> asking;  // the version a request the compiler has not answered yet was sent at
    bool edited { false };               // the text differs from what was opened or last saved: not one to keep on disk
    bool fromCache { false };            // read from disk (H2) and not yet answered by the compiler, which is asked to confirm
    bool cacheChecked { false };         // the disk was asked once for this open
    std::string writtenKey;              // the key the disk holds for this document, so the same answer is not written twice
    // What the client was given last: the version, the digest of the compiler's tokens in it, and whether they
    // were exact. A compiler answer that differs from it, for the same version, is a reason to refresh.
    std::optional<std::int64_t> givenVersion;
    std::uint64_t givenDigest { 0 };
    bool givenExact { false };
};

struct Counters {
    std::uint64_t exact { 0 };      // answered from tokens the compiler gave for this very text
    std::uint64_t standIn { 0 };    // answered at once from moved (or no) tokens, and refreshed when the compiler answers
    std::uint64_t waited { 0 };     // waited for the compiler: a client that cannot be refreshed, or nothing kept to stand in
    std::uint64_t refreshes { 0 };  // refreshes asked because the compiler's answer was not what the client had
    std::uint64_t dropped { 0 };    // compiler answers for a version whose edits are no longer known
    std::uint64_t cacheHits { 0 };
    std::uint64_t cacheMisses { 0 };
    std::uint64_t cacheWrites { 0 };
    std::deque<double> composeMs;   // the server's own work per answer: merge, cut, encode
};

class Store {
public:
    // How many changes of one document are kept for answers to requests sent before them.
    static constexpr std::size_t LOG_LIMIT { 512 };

private:
    struct Change {
        std::int64_t before { 0 };
        std::int64_t after { 0 };
        std::vector<Edit> edits;
    };
    std::map<std::string, Kept, std::less<>> kept_;
    std::map<std::string, std::deque<Change>, std::less<>> log_;

public:
    Counters counters;

    Kept* find(std::string_view uri);
    // The document's entry, made on its first request: edits are logged from then on.
    Kept& at(std::string_view uri);
    void forget(std::string_view uri);

    // The document changed from `before` to `version` by `edits`: what is kept moves with them and is no longer exact.
    void edited(std::string_view uri, std::int64_t before, std::int64_t version, std::span<const Edit> edits);

    // The compiler answered `core` for `answered`, the version it had; the document is at `current` now. The edits
    // since are applied; tokens for a version the log no longer reaches are dropped (false). `settled` is false while
    // the compiler still works on what the file reads, which may change its answer without an edit.
    bool engine_answered(std::string_view uri, std::int64_t answered, std::int64_t current, std::vector<Token> core, bool settled);

    // Something the compiler reads changed (a restart, modules prepared, another file edited, a new plan): every
    // kept answer still stands in, and none is exact.
    void unsettle_all();
    void unsettle_except(std::string_view uri);

    void composed(double milliseconds);
    Json report() const;
};

} // namespace mcppls::orchestrator::highlights
