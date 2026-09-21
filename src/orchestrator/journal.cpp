module mcppls.orchestrator.journal;

import std;
import nlohmann.json;

namespace mcppls::orchestrator {

using Json = nlohmann::json;

void Journal::add(std::string_view kind, Json detail) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    events_.push_back(Event { std::format("{:%FT%TZ}", now), std::string { kind }, std::move(detail) });
    if (events_.size() > CAPACITY) events_.pop_front();
    if (const auto it = totals_.find(kind); it != totals_.end()) ++it->second;
    else totals_.emplace(std::string { kind }, 1);
}

Json Journal::recent(std::size_t limit) const {
    Json events = Json::array();
    const std::size_t count { std::min(limit, events_.size()) };
    for (auto it = events_.end() - static_cast<std::ptrdiff_t>(count); it != events_.end(); ++it) {
        events.push_back(Json { { "at", it->at }, { "kind", it->kind }, { "detail", it->detail } });
    }
    return events;
}

std::size_t Journal::total(std::string_view kind) const {
    const auto it = totals_.find(kind);
    return it == totals_.end() ? 0 : it->second;
}

Json Journal::totals() const {
    Json totals = Json::object();
    for (const auto& [kind, count] : totals_) totals[kind] = count;
    return totals;
}

} // namespace mcppls::orchestrator
