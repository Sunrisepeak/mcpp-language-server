// The answer layer's state for one workspace (answer-layer design 2026-10-13, 4 and 6): what is kept, what changed
// since, what was learned of the types receivers have, what was served and how the engine's answer compared, and the
// files all of it is kept in between sessions. The workspace routes; this decides what a kept answer is worth, and
// composes it. Engine requests and the client are the workspace's: nothing here talks to a process.
export module mcppls.orchestrator.answering;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.orchestrator.answer;

export namespace mcppls::orchestrator::answering {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Settings {
    std::string cache { "on" };                            // on | memory | off (mcppls.answer.cache)
    std::chrono::milliseconds budget { 450 };              // mcppls.answer.budget
    bool prefetch { true };                                // mcppls.answer.prefetch
    answer::Marks marks { answer::Marks::subtle };         // mcppls.completion.provenance
    bool agentAuthoritative { false };                     // mcppls.answer.agent: authoritative
};

// One request, read: what is asked, where, and the keys it is kept under.
struct Asked {
    answer::Kind kind { answer::Kind::completion };
    answer::Context context;
    std::string uri;
    std::string path;
    std::string text;
    base::Position position;
    std::string fingerprint;
    std::string k1;
    std::optional<std::string> k3;
    std::string expanded;          // the qualifier with its aliases spelled out (K3)
    std::optional<std::string> declaredType;   // the receiver's type as the text spells it, when it does
};

struct Found {
    answer::Entry entry;
    std::string key;
    answer::Source source { answer::Source::k1 };
    answer::Certainty certainty { answer::Certainty::uncertain };
    bool providerChanged { false };
};

struct Served {
    Json result;
    answer::Provenance provenance;
    std::size_t doubted { 0 };
};

class Layer {
public:
    // `workspaceDirectory`: where this workspace's answers are kept; `namesDirectory`: where the names (K3) every
    // workspace shares are; `openText`: an open document's text by path, if open.
    Layer(Settings settings, std::string workspaceDirectory, std::string namesDirectory,
          std::function<std::optional<std::string>(std::string_view path)> openText);

    const Settings& settings() const { return settings_; }
    bool enabled() const { return settings_.cache != "off"; }

    // ---- what changed ----------------------------------------------------------------------------------------------
    // A file changed: edited (its new text), saved, or changed on disk.
    void changed(std::string_view path);
    // What the engine answers from changed as a whole (a new plan, a new engine): every answer kept before is uncertain.
    void everything_changed();

    // ---- one request ------------------------------------------------------------------------------------------------
    // nullopt when the request is not one the layer answers, or where it stands is not read for certain.
    std::optional<Asked> read(std::string_view method, std::string uri, std::string path, std::string text, const Json& params,
                              std::string fingerprint) const;
    // The best kept answer for it: K1, then K2 (a learned type), then K3.
    std::optional<Found> lookup(const Asked& asked);
    // A kept answer composed for where it is asked now; nullopt for a jump that is not certain and was not found again.
    std::optional<Served> serve(const Asked& asked, const Found& found) const;
    // The engine's answer: kept under every key it is good for. `served`, when a kept answer went out first, is what
    // the person saw (the difference is counted and said).
    void keep(const Asked& asked, const Json& result, const std::optional<Served>& served, std::chrono::milliseconds took);
    // What the engine's typeDefinition at the receiver said: the receiver's type, from now on known for this place and
    // for the type's spelling.
    void learn_type(const Asked& asked, std::string_view name, std::string_view path, int line);
    bool knows_type(const Asked& asked) const;

    // ---- what to ask the engine while it is idle (design 4.8) ---------------------------------------------------------
    // A place in `text` -- after a `.`, `->` or `::` that the file already has -- whose answer is not kept yet, nearest
    // `near` first; nullopt when there is none.
    std::optional<base::Position> prefetch_place(std::string_view uri, std::string_view text, base::Position near, std::string_view fingerprint);
    // A2 (plan 2026-10-14): a prefetch that came back with nothing (or failed). Its place is not asked again until a
    // file or the plan changes -- empty answers are not kept, so without this the same place was asked every 500 ms.
    void prefetch_came_back_empty(const Asked& asked);

    // ---- kept between sessions --------------------------------------------------------------------------------------
    void load();
    // Written when something changed and it is due (at most once a minute), or now (`force`, at shutdown).
    void save(bool force = false);

    // ---- what it did -------------------------------------------------------------------------------------------------
    void count_served(const Asked& asked, const Served& served, std::chrono::milliseconds took);
    void count_engine(const Asked& asked, std::chrono::milliseconds took);
    void count_background(std::string_view what);
    Json report() const;
    std::size_t entries() const { return store_.size(); }

    // The log line for an answer that went out (design 6.4); `info` only for what is worth reading.
    static std::string log_line(const Asked& asked, const Served& served, std::chrono::milliseconds took);

private:
    std::uint64_t generation_(std::string_view path) const;   // a file's own count of changes, or its disk stamp
    answer::Now now_(const Asked& asked) const;
    std::optional<std::string> text_of_(std::string_view path) const;

    Settings settings_;
    std::string workspaceFile_;
    std::string namesDirectory_;
    std::function<std::optional<std::string>(std::string_view)> openText_;
    answer::Store store_;
    std::unordered_map<std::string, std::uint64_t> changes_;   // path key -> how often it changed
    std::uint64_t total_ { 0 };                               // all changes (everything_changed() counts as many)
    // Learned: a place (K1) and a type's spelling -> the type's key, file and line.
    struct Type {
        std::string key;
        std::string path;
        std::uint64_t generation { 0 };
    };
    std::unordered_map<std::string, Type> placeTypes_;
    std::unordered_map<std::string, Type> spelledTypes_;
    std::set<std::string> namesLoaded_;   // fingerprints whose shared names were read
    std::unordered_map<std::string, std::string> fruitless_;   // K1 -> the state it came back empty in (A2)
    std::uint64_t savedAt_ { ~std::uint64_t { 0 } };
    std::optional<Clock::time_point> lastSave_;

    struct Counts {
        std::map<std::string, std::size_t, std::less<>> bySource;   // "completion cache:K1" -> n
        std::deque<double> servedMs;
        std::deque<double> engineMs;
        std::size_t replacedSame { 0 };
        std::size_t replacedDifferent { 0 };
        std::size_t itemsGone { 0 };
        std::size_t itemsAdded { 0 };
        std::size_t doubted { 0 };
        std::map<std::string, std::size_t, std::less<>> background;   // detached, prefetched, learned, dropped
    };
    Counts counts_;
};

// A file's visibility fingerprint (design 4.3): the toolchain's, the command's, what it imports, its preamble.
std::string fingerprint(std::string_view toolchain, std::span<const std::string> arguments, std::span<const std::string> imports,
                        std::string_view text);

} // namespace mcppls::orchestrator::answering
