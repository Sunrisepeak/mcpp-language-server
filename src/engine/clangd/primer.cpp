module mcppls.engine.clangd.primer;

import std;

namespace mcppls::engine::clangd {

void Primer::set_modules(std::vector<PrimeModule> modules) {
    std::map<std::string, State, std::less<>> states;
    modules_.clear();
    for (auto& module : modules) {
        const auto previous = states_.find(module.name);
        // Work in flight belongs to the old graph: it is started again if still wanted.
        states[module.name] = previous != states_.end() && previous->second == State::done ? State::done : State::unwanted;
        std::string name { module.name };
        modules_.emplace(std::move(name), std::move(module));
    }
    states_ = std::move(states);
    running_ = 0;
    heights_.clear();
}

bool Primer::same_modules(std::span<const PrimeModule> modules) const {
    if (modules.size() != modules_.size()) return false;
    return std::ranges::all_of(modules, [&](const PrimeModule& module) {
        const auto it = modules_.find(module.name);
        return it != modules_.end() && it->second.requires_ == module.requires_ && it->second.primeFile == module.primeFile;
    });
}

std::size_t Primer::want(std::span<const std::string> names) {
    std::size_t added { 0 };
    std::vector<std::string> pending { names.begin(), names.end() };
    while (!pending.empty()) {
        const std::string name { std::move(pending.back()) };
        pending.pop_back();
        const auto module = modules_.find(name);
        if (module == modules_.end()) continue;
        auto& state = states_[name];
        if (state != State::unwanted) continue;
        state = State::waiting;
        heights_.clear();
        ++added;
        for (const auto& required : module->second.requires_) pending.push_back(required);
    }
    return added;
}

bool Primer::imports_done_(const PrimeModule& module) const {
    return std::ranges::all_of(module.requires_, [&](const std::string& required) {
        const auto it = states_.find(required);
        return it == states_.end() || it->second == State::done;   // an import outside the graph cannot be waited for
    });
}

std::size_t Primer::height_(std::string_view name) const {
    if (heights_.empty()) {
        // Importers among the wanted modules, then the longest chain above each module, from the top down.
        std::map<std::string_view, std::vector<std::string_view>, std::less<>> importers;
        for (const auto& [module, state] : states_) {
            if (state == State::unwanted) continue;
            for (const auto& required : modules_.at(module).requires_) importers[required].push_back(module);
        }
        std::function<std::size_t(std::string_view, std::size_t)> visit = [&](std::string_view module, std::size_t guard) -> std::size_t {
            if (const auto known = heights_.find(module); known != heights_.end()) return known->second;
            std::size_t height { 0 };
            if (const auto above = importers.find(module); above != importers.end() && guard < states_.size()) {
                for (const auto importer : above->second) height = std::max(height, visit(importer, guard + 1) + 1);
            }
            heights_.emplace(std::string { module }, height);
            return height;
        };
        for (const auto& [module, state] : states_) {
            if (state != State::unwanted) (void)visit(module, 0);
        }
    }
    const auto it = heights_.find(name);
    return it == heights_.end() ? 0 : it->second;
}

std::vector<const PrimeModule*> Primer::start_ready(const std::function<bool(const PrimeModule&)>& built) {
    std::vector<const PrimeModule*> started;
    for (bool progressed { true }; progressed;) {
        progressed = false;
        std::vector<std::pair<std::size_t, const PrimeModule*>> ready;   // height, module
        for (auto& [name, state] : states_) {
            if (state != State::waiting) continue;
            const PrimeModule& module { modules_.at(name) };
            if (!imports_done_(module)) continue;
            if (module.primeFile.empty() || (built && built(module))) {
                // Not importable on its own (a partition), so its primary module's unit builds it;
                // or already built, so an importer's own build reuses it.
                state = State::done;
                progressed = true;
                continue;
            }
            ready.emplace_back(height_(name), &module);
        }
        if (progressed) continue;   // importers of what just completed may be ready too
        std::ranges::stable_sort(ready, std::greater {}, [](const auto& entry) { return entry.first; });
        for (const auto& [height, module] : ready) {
            if (running_ >= limit_) break;
            states_.at(module->name) = State::running;
            ++running_;
            started.push_back(module);
        }
    }
    return started;
}

void Primer::finish(std::string_view name) {
    const auto it = states_.find(name);
    if (it == states_.end() || it->second != State::running) return;
    it->second = State::done;
    --running_;
}

void Primer::reset() {
    for (auto& [name, state] : states_) state = State::unwanted;
    running_ = 0;
    heights_.clear();
}

Primer::State Primer::state(std::string_view name) const {
    const auto it = states_.find(name);
    return it == states_.end() ? State::unwanted : it->second;
}

const PrimeModule* Primer::find(std::string_view name) const {
    const auto it = modules_.find(name);
    return it == modules_.end() ? nullptr : &it->second;
}

bool Primer::busy() const {
    return std::ranges::any_of(states_, [](const auto& entry) { return entry.second == State::waiting || entry.second == State::running; });
}

std::pair<std::size_t, std::size_t> Primer::progress() const {
    std::size_t done { 0 };
    std::size_t wanted { 0 };
    for (const auto& [name, state] : states_) {
        if (state == State::unwanted) continue;
        ++wanted;
        if (state == State::done) ++done;
    }
    return { done, wanted };
}

} // namespace mcppls::engine::clangd
