// The moments of a workspace a report of a problem needs (robustness design O1): model loads, plans,
// engine starts, restarts, exits, timeouts, files set aside, module failures. The most recent are kept,
// with the time they happened and how many of each kind there were in all.
export module mcppls.orchestrator.journal;

import std;
import nlohmann.json;

export namespace mcppls::orchestrator {

class Journal {
public:
    static constexpr std::size_t CAPACITY { 500 };

    void add(std::string_view kind, nlohmann::json detail = nlohmann::json::object());
    // The most recent events, oldest first, at most `limit`.
    nlohmann::json recent(std::size_t limit) const;
    // How many events of `kind` there were since the workspace started, including those no longer kept.
    std::size_t total(std::string_view kind) const;
    nlohmann::json totals() const;

private:
    struct Event {
        std::string at;   // UTC, milliseconds
        std::string kind;
        nlohmann::json detail;
    };
    std::deque<Event> events_;
    std::map<std::string, std::size_t, std::less<>> totals_;
};

} // namespace mcppls::orchestrator
