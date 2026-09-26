module mcppls.engine.clangd.guard;

import std;

namespace mcppls::engine::clangd {

std::string_view to_string(RestartCause cause) {
    switch (cause) {
    case RestartCause::plan: return "plan";
    case RestartCause::recovery: return "recovery";
    case RestartCause::crash: return "crash";
    case RestartCause::user: return "user";
    }
    return "recovery";
}

GuardClock::time_point RestartGate::earliest(GuardClock::time_point now, RestartCause cause) const {
    if (cause == RestartCause::user) return now;
    const std::size_t count { recent(now, cause) };
    if (count == 0) return now;
    const auto last = restarts_.at(cause).back();
    const std::chrono::seconds gap { std::min<std::chrono::seconds::rep>(FIRST_GAP.count() << std::min<std::size_t>(count - 1, 16),
                                                                         std::chrono::duration_cast<std::chrono::seconds>(MAX_GAP).count()) };
    auto at = std::max(now, last + gap);
    if (cause != RestartCause::crash && count >= MAX_RESTARTS_PER_WINDOW) {
        const std::size_t step { std::min(count - MAX_RESTARTS_PER_WINDOW, BACKOFF.size() - 1) };
        at = std::max(at, last + BACKOFF[step]);
    }
    return at;
}

void RestartGate::record(GuardClock::time_point now, RestartCause cause) {
    if (cause == RestartCause::user) return;
    auto& restarts = restarts_[cause];
    restarts.push_back(now);
    while (!restarts.empty() && now - restarts.front() > WINDOW) restarts.pop_front();
}

std::size_t RestartGate::recent(GuardClock::time_point now, RestartCause cause) const {
    const auto restarts = restarts_.find(cause);
    if (restarts == restarts_.end()) return 0;
    return static_cast<std::size_t>(std::ranges::count_if(restarts->second, [&](GuardClock::time_point at) { return now - at <= WINDOW; }));
}

bool RestartGate::at_cap(GuardClock::time_point now, RestartCause cause) const {
    return cause != RestartCause::user && cause != RestartCause::crash && recent(now, cause) >= MAX_RESTARTS_PER_WINDOW;
}

void RestartGate::reset() { restarts_.clear(); }

std::set<std::string> doomed_modules(const std::map<std::string, std::vector<std::string>, std::less<>>& imports, std::string_view failed) {
    // Each module's importers, then one breadth-first walk from the failed module up through them.
    std::map<std::string_view, std::vector<std::string_view>> importers;
    for (const auto& [module, imported] : imports) {
        for (const auto& name : imported) importers[name].push_back(module);
    }
    std::set<std::string> doomed { std::string { failed } };
    std::deque<std::string_view> pending { failed };
    while (!pending.empty()) {
        const auto next = importers.find(pending.front());
        pending.pop_front();
        if (next == importers.end()) continue;
        for (const auto importer : next->second) {
            if (doomed.emplace(importer).second) pending.push_back(importer);
        }
    }
    return doomed;
}

Quarantine::Verdict Quarantine::timed_out(std::string_view uri, GuardClock::time_point sent, GuardClock::time_point now,
                                         std::optional<GuardClock::time_point> lastAnswer, bool rebuilding) {
    while (!unanswered_.empty() && now - std::get<0>(unanswered_.front()) > STALL_WINDOW) unanswered_.pop_front();
    // A request sent before the file was set aside, timing out after: nothing new about the file, nor about clangd.
    if (contains(uri)) return Verdict::wait;
    if (lastAnswer && *lastAnswer >= sent) {
        // clangd kept answering others while this file waited: the file is what is stuck -- unless it is rebuilding.
        if (rebuilding) return Verdict::wait;
        auto& entry = entries_[std::string { uri }];
        if (++entry.timeouts < TIMEOUTS_BEFORE_QUARANTINE) return Verdict::wait;
        put(uri, now);
        return Verdict::quarantined;
    }
    unanswered_.emplace_back(now, sent, std::string { uri });
    std::set<std::string_view> files;
    for (const auto& [at, requested, file] : unanswered_) files.insert(file);
    if (files.size() >= 2) return Verdict::stalled;
    if (rebuilding) return Verdict::wait;
    auto& entry = entries_[std::string { uri }];
    if (++entry.timeouts < TIMEOUTS_BEFORE_QUARANTINE) return Verdict::wait;
    put(uri, now);
    return Verdict::quarantined;
}

void Quarantine::answered(std::string_view uri) {
    if (const auto it = entries_.find(uri); it != entries_.end() && !it->second.until) it->second.timeouts = 0;
}

bool Quarantine::contains(std::string_view uri) const {
    const auto it = entries_.find(uri);
    return it != entries_.end() && it->second.until.has_value();
}

void Quarantine::put(std::string_view uri, GuardClock::time_point now) {
    auto& entry = entries_[std::string { uri }];
    const std::chrono::minutes term { std::min<std::chrono::minutes::rep>(FIRST_TERM.count() << std::min(entry.terms, 8), MAX_TERM.count()) };
    entry.until = now + term;
    entry.timeouts = 0;
    ++entry.terms;
    std::erase_if(unanswered_, [&](const auto& item) { return std::get<2>(item) == uri; });
}

bool Quarantine::release(std::string_view uri) {
    const auto it = entries_.find(uri);
    if (it == entries_.end() || !it->second.until) return false;
    it->second.until.reset();
    return true;
}

void Quarantine::release_all() {
    for (auto& [uri, entry] : entries_) {
        entry.until.reset();
        entry.timeouts = 0;
    }
    unanswered_.clear();
}

std::vector<std::string> Quarantine::due(GuardClock::time_point now) {
    std::vector<std::string> released;
    for (auto& [uri, entry] : entries_) {
        if (entry.until && *entry.until <= now) {
            entry.until.reset();
            released.push_back(uri);
        }
    }
    return released;
}

std::optional<std::string> Quarantine::first_stalled() const {
    if (unanswered_.empty()) return std::nullopt;
    const auto first = std::ranges::min_element(unanswered_, {}, [](const auto& item) { return std::get<1>(item); });
    return std::get<2>(*first);
}

std::size_t Quarantine::size() const {
    return static_cast<std::size_t>(std::ranges::count_if(entries_, [](const auto& item) { return item.second.until.has_value(); }));
}

std::vector<std::string> Quarantine::members() const {
    std::vector<std::string> files;
    for (const auto& [uri, entry] : entries_) {
        if (entry.until) files.push_back(uri);
    }
    return files;
}

void SpinWatch::state(std::string_view uri, std::string_view state, GuardClock::time_point now) {
    auto& file = files_[std::string { uri }];
    const bool building { state.find("parsing main file") != std::string_view::npos };
    if (building && !file.buildingSince) {
        file.buildingSince = now;
        file.buildingHash = file.lastHash;
        file.buildingSentAt = file.lastSentAt;
        file.reported = false;
    } else if (!building && file.buildingSince) {
        file.lastBuild = now - *file.buildingSince;
        file.buildingSince.reset();
    }
}

void SpinWatch::sent(std::string_view uri, std::size_t textHash, GuardClock::time_point now) {
    auto& file = files_[std::string { uri }];
    file.lastDemand = now;
    file.lastSentAt = now;
    file.lastHash = textHash;
}

void SpinWatch::asked(std::string_view uri, GuardClock::time_point now) {
    const auto it = files_.find(uri);
    if (it != files_.end()) it->second.lastDemand = now;
}

void SpinWatch::forget(std::string_view uri) {
    if (const auto it = files_.find(uri); it != files_.end()) files_.erase(it);
}

void SpinWatch::restarted() {
    for (auto& [uri, file] : files_) {
        file.buildingSince.reset();
        file.reported = false;
    }
}

bool SpinWatch::waited_on_(const File& file) {
    // A request asked, or a version sent, after the version being built was sent waits on this build (a
    // request is often sent before clangd says it started building).
    if (!file.buildingSince || !file.lastDemand) return false;
    return file.buildingSentAt ? *file.lastDemand > *file.buildingSentAt : *file.lastDemand > *file.buildingSince;
}

std::optional<GuardClock::time_point> SpinWatch::due_(const File& file) {
    if (!file.buildingSince || !file.lastBuild || file.reported) return std::nullopt;
    const GuardClock::duration budget { std::max<GuardClock::duration>(MIN_BUDGET, *file.lastBuild * HISTORY_FACTOR) };
    return *file.buildingSince + budget;
}

std::vector<SpinWatch::Spin> SpinWatch::check(GuardClock::time_point now) {
    std::vector<Spin> spins;
    for (auto& [uri, file] : files_) {
        const auto due = due_(file);
        // Past its budget, and the editor has asked for something since the version being built: that waits behind it.
        if (!due || now < *due || !waited_on_(file)) continue;
        file.reported = true;
        spins.push_back(Spin { uri, std::chrono::duration_cast<std::chrono::milliseconds>(now - *file.buildingSince),
                               std::chrono::duration_cast<std::chrono::milliseconds>(*due - *file.buildingSince), file.buildingHash });
    }
    return spins;
}

std::optional<GuardClock::time_point> SpinWatch::next_due() const {
    // Only builds something waits behind: a deadline check() would not act on would come back at
    // once, forever. A version sent later is an event, and the timers run after every event.
    std::optional<GuardClock::time_point> earliest;
    for (const auto& [uri, file] : files_) {
        if (!waited_on_(file)) continue;
        const auto due = due_(file);
        if (due && (!earliest || *due < *earliest)) earliest = due;
    }
    return earliest;
}

void StuckWatch::suspect(GuardClock::time_point now, std::optional<double> cpuSeconds) {
    if (since_ || !cpuSeconds) return;
    since_.emplace(now, *cpuSeconds);
}

void StuckWatch::clear() { since_.reset(); }

bool StuckWatch::watching() const { return since_.has_value(); }

std::optional<GuardClock::time_point> StuckWatch::started() const {
    if (!since_) return std::nullopt;
    return since_->first;
}

std::optional<GuardClock::time_point> StuckWatch::due() const {
    if (!since_) return std::nullopt;
    return since_->first + window_;
}

StuckWatch::Verdict StuckWatch::check(GuardClock::time_point now, std::optional<double> cpuSeconds) {
    if (!since_ || now < since_->first + window_) return {};
    const auto since = std::exchange(since_, std::nullopt);
    if (!cpuSeconds) return {};
    const double seconds { std::chrono::duration<double>(now - since->first).count() };
    const double used { std::max(0.0, *cpuSeconds - since->second) };
    return Verdict { used < IDLE_SHARE * seconds, seconds, used };
}

LineLimiter::Decision LineLimiter::admit(GuardClock::time_point now) {
    Decision decision;
    if (!windowStart_ || now - *windowStart_ >= window_) {
        windowStart_ = now;
        inWindow_ = 0;
        decision.suppressedBefore = std::exchange(suppressed_, 0);
    }
    if (inWindow_ < burst_) {
        ++inWindow_;
        decision.forward = true;
    } else {
        ++suppressed_;
        decision.suppressedBefore = 0;
    }
    return decision;
}

bool engine_working(std::string_view state) {
    for (std::size_t start { 0 }; start <= state.size();) {
        const std::size_t comma { state.find(',', start) };
        std::string_view part { state.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start) };
        while (part.starts_with(' ')) part.remove_prefix(1);
        while (part.ends_with(' ')) part.remove_suffix(1);
        if (!part.empty() && part != "idle" && !part.ends_with("queued") && !part.ends_with("(queued)")) return true;
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return false;
}

std::size_t engine_workers(std::size_t hardwareThreads, bool macos) {
    const std::size_t threads { std::max<std::size_t>(1, hardwareThreads) };
    const std::size_t cores { macos ? threads : std::max<std::size_t>(1, threads / 2) };
    return std::max<std::size_t>(2, cores / 4);
}

std::size_t preparation_limit(std::size_t hardwareThreads, bool macos, std::size_t waitingFiles,
                              bool waitingOnPreparation) {
    const std::size_t workers { engine_workers(hardwareThreads, macos) };
    const bool throttle { waitingFiles > 0 && !waitingOnPreparation };
    return std::max<std::size_t>(1, throttle ? workers / 4 : workers / 2);
}

} // namespace mcppls::engine::clangd
