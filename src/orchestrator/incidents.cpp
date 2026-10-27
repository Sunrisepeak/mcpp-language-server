module mcppls.orchestrator.incidents;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.engine;
import mcpplibs.os;

namespace os = mcpplibs::os;

namespace mcppls::orchestrator::incidents {

using Json = nlohmann::json;

namespace {

// The threads that used the most CPU over `window`, most first: what a spinning clangd shows as one worker
// at a full core ("Worker:main.cpp" in the hello project, fix plan F16).
Json busiest_threads(std::int64_t pid, std::chrono::milliseconds window) {
    const auto seconds_of = [](const os::process::thread_info& thread) { return static_cast<double>(thread.cpu_ns) / 1e9; };
    const auto before = os::process::threads(static_cast<std::uint64_t>(pid));
    if (before.empty()) return nullptr;
    std::this_thread::sleep_for(window);
    const auto after = os::process::threads(static_cast<std::uint64_t>(pid));
    std::map<std::uint64_t, double> start;
    for (const auto& thread : before) start[thread.id] = seconds_of(thread);
    std::vector<std::pair<double, const os::process::thread_info*>> used;
    for (const auto& thread : after) {
        const auto was = start.find(thread.id);
        used.emplace_back(seconds_of(thread) - (was == start.end() ? 0.0 : was->second), &thread);
    }
    std::ranges::sort(used, std::greater {}, &std::pair<double, const os::process::thread_info*>::first);
    Json threads = Json::array();
    for (const auto& [seconds, thread] : used | std::views::take(8)) {
        threads.push_back(Json { { "name", thread->name }, { "cpuSecondsInWindow", std::round(seconds * 100) / 100 }, { "cpuSecondsTotal", seconds_of(*thread) } });
    }
    return Json { { "windowSeconds", std::chrono::duration<double>(window).count() }, { "threads", std::move(threads) }, { "count", after.size() } };
}

} // namespace

std::string directory_name(std::string_view kind, std::chrono::system_clock::time_point at) {
    std::string safe;
    for (const char c : kind) safe += std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ? c : '-';
    return std::format("{:%Y%m%dT%H%M%S}Z-{}", std::chrono::floor<std::chrono::milliseconds>(at), safe);
}

std::optional<std::chrono::system_clock::time_point> time_of(std::string_view name) {
    // yyyyMMddTHHmmss.SSSZ-
    if (name.size() < 21 || name[8] != 'T' || name[15] != '.' || name[19] != 'Z' || name[20] != '-') return std::nullopt;
    const auto number = [&](std::size_t at, std::size_t length) -> std::optional<int> {
        int value { 0 };
        const auto [end, error] = std::from_chars(name.data() + at, name.data() + at + length, value);
        if (error != std::errc {} || end != name.data() + at + length) return std::nullopt;
        return value;
    };
    const auto year = number(0, 4), month = number(4, 2), day = number(6, 2), hour = number(9, 2), minute = number(11, 2), second = number(13, 2);
    if (!year || !month || !day || !hour || !minute || !second) return std::nullopt;
    const std::chrono::year_month_day date { std::chrono::year { *year }, std::chrono::month { static_cast<unsigned>(*month) },
                                             std::chrono::day { static_cast<unsigned>(*day) } };
    if (!date.ok()) return std::nullopt;
    return std::chrono::sys_days { date } + std::chrono::hours { *hour } + std::chrono::minutes { *minute } + std::chrono::seconds { *second };
}

void prune(std::string_view incidentsDirectory, std::chrono::system_clock::time_point now) {
    std::error_code ignored;
    std::vector<std::pair<std::string, std::chrono::system_clock::time_point>> found;
    for (auto& path : base::children(incidentsDirectory)) {
        if (const auto at = time_of(base::file_name(path))) found.emplace_back(std::move(path), *at);
    }
    std::ranges::sort(found, std::greater {}, [](const auto& item) { return base::file_name(item.first); });
    for (std::size_t i { 0 }; i < found.size(); ++i) {
        if (i >= KEEP || now - found[i].second > MAX_AGE) std::filesystem::remove_all(found[i].first, ignored);
    }
}

base::Result<std::string> write(std::string_view cacheDirectory, std::string_view kind, Json incident, std::vector<engine::IncidentFile> files,
                                std::optional<std::int64_t> pid) {
    const auto now = std::chrono::system_clock::now();
    const std::string root { base::join_path(cacheDirectory, DIRECTORY) };
    const std::string directory { base::join_path(root, directory_name(kind, now)) };
    if (std::error_code failed; !std::filesystem::create_directories(directory, failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", directory, failed.message()));
    if (pid) {
        if (Json threads = busiest_threads(*pid, std::chrono::seconds { 1 }); !threads.is_null()) incident["engineThreads"] = std::move(threads);
    }
    Json attached = Json::array();
    for (const auto& file : files) {
        const std::string name { base::file_name(file.name) };   // never outside the incident's directory
        if (name.empty() || name == "incident.json") continue;
        if (os::fs::write(base::join_path(directory, name), file.content)) attached.push_back(name);
    }
    incident["attached"] = std::move(attached);
    if (auto written = os::fs::write(base::join_path(directory, "incident.json"), incident.dump(2)); !written) {
        return base::fail("write-file", written.error());
    }
    prune(root, now);
    return directory;
}

} // namespace mcppls::orchestrator::incidents
