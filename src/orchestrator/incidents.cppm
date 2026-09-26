// Incidents (fix plan F17.2, F17.7): what went wrong, written down when it happened, so one occurrence is
// enough to see why. Each is a directory of a workspace's cache, `incidents/<UTC time>-<kind>/`, holding
// `incident.json` and the files an engine attached (its own log, say), which `attached` lists. The newest
// KEEP stay, none older than MAX_AGE. A diagnostic bundle (fix plan F18) carries them; nothing here ever
// leaves the machine.
export module mcppls.orchestrator.incidents;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.engine;

export namespace mcppls::orchestrator::incidents {

inline constexpr std::size_t KEEP { 20 };
inline constexpr std::chrono::hours MAX_AGE { 24 * 7 };
inline constexpr std::string_view DIRECTORY { "incidents" };

// "20260926T071946.638Z-engine-crash": sorts by time; the kind keeps only letters, digits and '-'.
std::string directory_name(std::string_view kind, std::chrono::system_clock::time_point at);
// When a directory named by directory_name was made; nullopt for any other name.
std::optional<std::chrono::system_clock::time_point> time_of(std::string_view name);

// Writes one incident under `<cacheDirectory>/incidents/`, then prunes. With `pid`, the incident also says
// what each thread of that process used of the CPU over one second (Linux; platform::thread_cpu), sampled
// here, so this runs off the event loop. Returns the incident's directory.
base::Result<std::string> write(std::string_view cacheDirectory, std::string_view kind, nlohmann::json incident,
                                std::vector<engine::IncidentFile> files, std::optional<std::int64_t> pid);

// Removes all but the newest KEEP incidents, and every one older than MAX_AGE at `now`.
void prune(std::string_view incidentsDirectory, std::chrono::system_clock::time_point now);

} // namespace mcppls::orchestrator::incidents
