module mcppls.orchestrator.incidents;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.engine;

namespace mcppls::orchestrator::incidents {

using Json = nlohmann::json;

namespace {

// The threads that used the most CPU over `window`, most first: what a spinning clangd shows as one worker
// at a full core ("Worker:main.cpp" in the hello project, fix plan F16).
Json busiest_threads(std::int64_t pid, std::chrono::milliseconds window) {
    const auto before = platform::thread_cpu(pid);
    if (before.empty()) return nullptr;
    std::this_thread::sleep_for(window);
    const auto after = platform::thread_cpu(pid);
    std::map<std::int64_t, double> start;
    for (const auto& thread : before) start[thread.id] = thread.seconds;
    std::vector<std::pair<double, const platform::ThreadCpu*>> used;
    for (const auto& thread : after) {
        const auto was = start.find(thread.id);
        used.emplace_back(thread.seconds - (was == start.end() ? 0.0 : was->second), &thread);
    }
    std::ranges::sort(used, std::greater {}, &std::pair<double, const platform::ThreadCpu*>::first);
    Json threads = Json::array();
    for (const auto& [seconds, thread] : used | std::views::take(8)) {
        threads.push_back(Json { { "name", thread->name }, { "cpuSecondsInWindow", std::round(seconds * 100) / 100 }, { "cpuSecondsTotal", thread->seconds } });
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
    std::vector<std::pair<std::string, std::chrono::system_clock::time_point>> found;
    for (auto& path : platform::fs::list_directory(incidentsDirectory)) {
        if (const auto at = time_of(base::file_name(path))) found.emplace_back(std::move(path), *at);
    }
    std::ranges::sort(found, std::greater {}, [](const auto& item) { return base::file_name(item.first); });
    for (std::size_t i { 0 }; i < found.size(); ++i) {
        if (i >= KEEP || now - found[i].second > MAX_AGE) platform::fs::remove_all(found[i].first);
    }
}

base::Result<std::string> write(std::string_view cacheDirectory, std::string_view kind, Json incident, std::vector<engine::IncidentFile> files,
                                std::optional<std::int64_t> pid) {
    const auto now = std::chrono::system_clock::now();
    const std::string root { base::join_path(cacheDirectory, DIRECTORY) };
    const std::string directory { base::join_path(root, directory_name(kind, now)) };
    if (auto created = platform::fs::create_directories(directory); !created) return std::unexpected { created.error() };
    if (pid) {
        if (Json threads = busiest_threads(*pid, std::chrono::seconds { 1 }); !threads.is_null()) incident["engineThreads"] = std::move(threads);
    }
    Json attached = Json::array();
    for (const auto& file : files) {
        const std::string name { base::file_name(file.name) };   // never outside the incident's directory
        if (name.empty() || name == "incident.json") continue;
        if (platform::fs::write_file(base::join_path(directory, name), file.content)) attached.push_back(name);
    }
    incident["attached"] = std::move(attached);
    if (auto written = platform::fs::write_file(base::join_path(directory, "incident.json"), incident.dump(2)); !written) {
        return std::unexpected { written.error() };
    }
    prune(root, now);
    return directory;
}

} // namespace mcppls::orchestrator::incidents
