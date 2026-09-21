module mcppls.devtools.bench.tasks;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;

namespace mcppls::devtools::bench {
namespace {

namespace fs = mcppls::platform::fs;
namespace toolrun = mcppls::platform::toolrun;

constexpr std::array<std::string_view, 7> REQUIRED_TASK_FIELDS {
    "id", "title", "prompt", "project", "check", "reference", "shape",
};
constexpr std::array<std::string_view, 2> VALID_SHAPES { "split", "all-cppm" };

bool is_valid_shape(std::string_view shape) {
    return std::ranges::any_of(VALID_SHAPES, [&](std::string_view s) { return s == shape; });
}

base::Result<ContentCheck> parse_content_check(const nlohmann::json& value) {
    if (!value.is_object()) return base::fail("bench-task", "'file-contains' must be an object");
    if (!value.contains("files") || !value.at("files").is_array() || value.at("files").empty()) {
        return base::fail("bench-task", "'file-contains.files' must be a non-empty list of strings");
    }
    ContentCheck check;
    for (const auto& entry : value.at("files")) {
        if (!entry.is_string()) return base::fail("bench-task", "'file-contains.files' must be a list of strings");
        check.files.push_back(entry.get<std::string>());
    }
    if (value.contains("all")) {
        if (!value.at("all").is_array()) return base::fail("bench-task", "'file-contains.all' must be a list of strings");
        for (const auto& entry : value.at("all")) {
            if (!entry.is_string()) return base::fail("bench-task", "'file-contains.all' must be a list of strings");
            check.all.push_back(entry.get<std::string>());
        }
    }
    if (value.contains("none")) {
        if (!value.at("none").is_array()) return base::fail("bench-task", "'file-contains.none' must be a list of strings");
        for (const auto& entry : value.at("none")) {
            if (!entry.is_string()) return base::fail("bench-task", "'file-contains.none' must be a list of strings");
            check.none.push_back(entry.get<std::string>());
        }
    }
    if (check.all.empty() && check.none.empty()) {
        return base::fail("bench-task", "'file-contains' needs at least one of 'all' or 'none'");
    }
    return check;
}

CheckOutcome evaluate_content_check(const ContentCheck& check, const std::string& cwd) {
    std::string concatenated;
    for (const auto& relative : check.files) {
        const std::string full { base::join_path(cwd, relative) };
        auto text = fs::read_file(full);
        if (!text) return { false, std::format("file-contains: cannot read {}: {}", full, text.error().message) };
        concatenated += *text;
    }
    std::vector<std::string> missing;
    for (const auto& needle : check.all) if (!concatenated.contains(needle)) missing.push_back(needle);
    std::vector<std::string> present;
    for (const auto& needle : check.none) if (concatenated.contains(needle)) present.push_back(needle);
    if (missing.empty() && present.empty()) return { true, "" };
    std::string detail { std::format("file-contains {}:", base::join(check.files, ", ")) };
    if (!missing.empty()) detail += std::format(" missing {};", base::join(missing, ", "));
    if (!present.empty()) detail += std::format(" present (should be absent) {};", base::join(present, ", "));
    return { false, detail };
}

std::string describe_command_failure(const std::vector<std::string>& command, bool timedOut, int exitCode,
                                     int timeoutSeconds, const std::string& output, const std::string& error) {
    const std::string where { base::join(command, " ") };
    const std::string combined { output + "\n" + error };
    const std::string tail { mcppls::platform::last_lines(combined, 15) };
    if (timedOut) return std::format("`{}` timed out after {}s\n{}", where, timeoutSeconds, tail);
    return std::format("`{}` exited {}\n{}", where, exitCode, tail);
}

} // namespace

base::Result<Check> parse_check(const nlohmann::json& value) {
    if (value.is_array()) {
        if (value.empty()) return base::fail("bench-task", "a 'check' command must be a non-empty list of strings");
        Check check;
        for (const auto& part : value) {
            if (!part.is_string()) return base::fail("bench-task", "a 'check' command must be a list of strings");
            check.command.push_back(part.get<std::string>());
        }
        return check;
    }
    if (value.is_object() && value.contains("file-contains")) {
        auto content = parse_content_check(value.at("file-contains"));
        if (!content) return std::unexpected { content.error() };
        Check check;
        check.content = std::move(*content);
        return check;
    }
    return base::fail("bench-task", std::format("each 'check' entry must be a command (list of strings) or "
                                                 "a declarative check object, got {}", value.dump()));
}

base::Result<Task> load_task(const std::string& taskDir) {
    const std::string taskJson { base::join_path(taskDir, "task.json") };
    if (!fs::is_regular_file(taskJson)) return base::fail("bench-task", std::format("{}: no task.json", taskDir));
    auto text = fs::read_file(taskJson);
    if (!text) return std::unexpected { text.error() };
    nlohmann::json raw;
    try {
        raw = nlohmann::json::parse(*text);
    } catch (const std::exception& error) {
        return base::fail("bench-task", std::format("{}: invalid JSON ({})", taskJson, error.what()));
    }

    std::vector<std::string_view> missing;
    for (const auto field : REQUIRED_TASK_FIELDS) if (!raw.contains(field)) missing.push_back(field);
    if (!missing.empty()) {
        return base::fail("bench-task", std::format("{}: missing field(s) [{}]", taskJson, base::join(
            std::vector<std::string> { missing.begin(), missing.end() }, ", ")));
    }

    const std::string id { raw.at("id").get<std::string>() };
    const std::string normalizedDir { base::normalize_path(taskDir) };
    const std::string_view dirName { base::file_name(normalizedDir) };
    if (id != dirName) {
        return base::fail("bench-task", std::format("{}: id '{}' != directory name '{}'", taskJson, id, dirName));
    }
    const std::string shape { raw.at("shape").get<std::string>() };
    if (!is_valid_shape(shape)) {
        return base::fail("bench-task", std::format("{}: shape must be 'split' or 'all-cppm', got '{}'", taskJson, shape));
    }
    if (!raw.at("check").is_array() || raw.at("check").empty()) {
        return base::fail("bench-task", std::format("{}: 'check' must be a non-empty list", taskJson));
    }

    Task task;
    task.id = id;
    task.title = raw.at("title").get<std::string>();
    task.prompt = raw.at("prompt").get<std::string>();
    task.shape = shape;
    if (raw.contains("toolchain") && raw.at("toolchain").is_string()) task.toolchain = raw.at("toolchain").get<std::string>();
    task.dir = taskDir;

    for (const auto& entry : raw.at("check")) {
        auto check = parse_check(entry);
        if (!check) return base::fail("bench-task", std::format("{}: {}", taskJson, check.error().message));
        task.checks.push_back(std::move(*check));
    }

    task.projectDir = base::join_path(taskDir, raw.at("project").get<std::string>());
    if (!fs::is_directory(task.projectDir)) {
        return base::fail("bench-task", std::format("{}: project directory {} does not exist", taskJson, task.projectDir));
    }
    if (!fs::is_regular_file(base::join_path(task.projectDir, "mcpp.toml"))) {
        return base::fail("bench-task", std::format("{}: {} has no mcpp.toml", taskJson, task.projectDir));
    }
    task.referencePath = base::join_path(taskDir, raw.at("reference").get<std::string>());
    if (!fs::is_regular_file(task.referencePath)) {
        return base::fail("bench-task", std::format("{}: reference patch {} does not exist", taskJson, task.referencePath));
    }
    return task;
}

base::Result<std::vector<Task>> discover_tasks(const std::string& tasksDir, std::optional<std::string> only) {
    std::vector<std::string> taskDirs;
    for (const auto& child : fs::list_directory(tasksDir)) {
        if (fs::is_regular_file(base::join_path(child, "task.json"))) taskDirs.push_back(child);
    }
    std::ranges::sort(taskDirs);
    if (only) {
        std::erase_if(taskDirs, [&](const std::string& d) { return base::file_name(base::normalize_path(d)) != *only; });
        if (taskDirs.empty()) return base::fail("bench-task", std::format("no task named '{}' under {}", *only, tasksDir));
    }
    std::vector<Task> tasks;
    for (const auto& dir : taskDirs) {
        auto task = load_task(dir);
        if (!task) return std::unexpected { task.error() };
        tasks.push_back(std::move(*task));
    }
    return tasks;
}

CheckOutcome run_checks(const std::vector<Check>& checks, const std::string& cwd, int timeoutSeconds) {
    for (const auto& check : checks) {
        if (check.content) {
            auto outcome = evaluate_content_check(*check.content, cwd);
            if (!outcome.ok) return outcome;
            continue;
        }
        // toolrun needs an absolute program path (the shell's own PATH search does not apply
        // here); every check command names a program by its bare name (`mcpp`, `python3`, ...).
        auto resolved = mcppls::platform::env::find_executable(check.command.front());
        if (!resolved) {
            return { false, std::format("`{}` is not on PATH", check.command.front()) };
        }
        std::vector<std::string> arguments { check.command.begin() + 1, check.command.end() };
        auto result = toolrun::run({
            .program = *resolved,
            .arguments = arguments,
            .workDirectory = cwd,
            .purpose = "bench",
            .network = toolrun::Network::allowed,
            .bounds = mcppls::platform::RunBounds { .hard = std::chrono::seconds { timeoutSeconds } },
        });
        if (!result) {
            return { false, std::format("`{}` could not start: {}", base::join(check.command, " "), result.error().message) };
        }
        if (result->exitCode != 0 || result->timedOut) {
            return { false, describe_command_failure(check.command, result->timedOut, result->exitCode,
                                                      timeoutSeconds, result->output, result->error) };
        }
    }
    return { true, "" };
}

base::Result<void> copy_tree(const std::string& source, const std::string& destination,
                             const std::function<bool(std::string_view name)>& skip) {
    if (auto created = fs::create_directories(destination); !created) return std::unexpected { created.error() };
    for (const auto& child : fs::list_directory(source)) {
        // `list_directory` already returns full, joined paths (never join them onto `source`
        // again -- repository convention).
        const std::string normalized { base::normalize_path(child) };
        const std::string_view name { base::file_name(normalized) };
        if (skip && skip(name)) continue;
        const std::string destChild { base::join_path(destination, std::string { name }) };
        if (fs::is_directory(child)) {
            if (auto copied = copy_tree(child, destChild, skip); !copied) return copied;
        } else if (fs::is_regular_file(child)) {
            auto content = fs::read_file(child);
            if (!content) return std::unexpected { content.error() };
            if (auto written = fs::write_file(destChild, *content); !written) return std::unexpected { written.error() };
        }
    }
    return {};
}

base::Result<void> copy_project(const std::string& projectDir, const std::string& dest) {
    auto copied = copy_tree(projectDir, dest, [](std::string_view name) {
        return name == "target" || name == "compile_commands.json";
    });
    if (!copied) return copied;
    return {};
}

} // namespace mcppls::devtools::bench
