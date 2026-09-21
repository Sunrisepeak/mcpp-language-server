// Parallel module preparation (usable plan W7). clangd builds the modules a file
// imports one after another in that file's worker. The primer schedules the same
// work across workers instead: for each module whose own imports are ready it
// opens a one-line unit, `import M;`, so independent modules of the graph build at
// the same time and the file the person opened finds them in clangd's cache.
export module mcppls.engine.clangd.primer;

import std;

export namespace mcppls::engine::clangd {

struct PrimeModule {
    std::string name;
    std::vector<std::string> requires_;   // module names this module imports
    std::string primeFile;                // the `import M;` unit; empty when the module cannot be imported directly
};

class Primer {
public:
    enum class State { unwanted, waiting, running, done };

private:
    std::map<std::string, PrimeModule, std::less<>> modules_;
    std::map<std::string, State, std::less<>> states_;
    std::size_t limit_ { 4 };
    std::size_t running_ { 0 };
    // How many wanted modules are stacked above each wanted module, on its longest chain of
    // importers: a module that many others wait on starts first. Recomputed after want().
    mutable std::map<std::string, std::size_t, std::less<>> heights_;

    bool imports_done_(const PrimeModule& module) const;
    std::size_t height_(std::string_view name) const;

public:
    // A new graph; modules already done keep that state when they are still in it.
    void set_modules(std::vector<PrimeModule> modules);
    // Whether `modules` is the graph already set: same names, imports and units.
    bool same_modules(std::span<const PrimeModule> modules) const;
    void set_limit(std::size_t limit) { limit_ = std::max<std::size_t>(1, limit); }
    // Wants `names` and everything they import, transitively. Returns how many modules became wanted.
    std::size_t want(std::span<const std::string> names);
    // Modules to start now: wanted, not started, every import done, within the limit, the ones
    // with the most modules waiting above them first. They are marked running. A ready module
    // `built` says the engine already has (its cached BMI, on a warm start) completes at once,
    // like a partition, and needs no unit.
    std::vector<const PrimeModule*> start_ready(const std::function<bool(const PrimeModule&)>& built = {});
    // A running module finished (built or failed; either way importers may proceed).
    void finish(std::string_view name);
    // Everything forgotten, as after an engine restart: nothing is running and nothing is done.
    void reset();

    State state(std::string_view name) const;
    const PrimeModule* find(std::string_view name) const;
    bool busy() const;                         // something wanted is not done
    std::pair<std::size_t, std::size_t> progress() const;   // done, wanted
    std::size_t running() const { return running_; }
};

} // namespace mcppls::engine::clangd
