// The agent task benchmark's tasks (tests/bench/README.md): a task and its checks. A check is an
// argv command or a declarative content check (`file-contains`); a task's checks run in order and
// stop at the first that fails.
export module mcppls.harness.tasks;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::harness {

// A declarative content check: every listed file's contents, concatenated in order (no
// separator, matching how the Python checks it replaces concatenated more than one file), must
// contain every string in `all` and none of the strings in `none`.
struct ContentCheck {
    std::vector<std::string> files;
    std::vector<std::string> all;
    std::vector<std::string> none;
};

// One `check` entry: either an argv command (`command` non-empty) or a declarative content
// check (`content` set). Never both.
struct Check {
    std::vector<std::string> command;
    std::optional<ContentCheck> content;
};

struct Task {
    std::string id;
    std::string title;
    std::string prompt;
    std::vector<Check> checks;
    std::string shape;
    std::optional<std::string> toolchain;
    std::string dir;
    std::string projectDir;
    std::string referencePath;
};

// A task.json (or its project/reference files) is malformed.
base::Result<Task> load_task(const std::string& taskDir);

// Every task under `tasksDir` (each `<tasksDir>/<id>/task.json`), sorted by id. `only` restricts
// to one task id; an error names it if it does not exist.
base::Result<std::vector<Task>> discover_tasks(const std::string& tasksDir, std::optional<std::string> only);

// One check's JSON form (an argv array or a `{"file-contains": {...}}` object) parsed and
// validated the way `load_task` validates every entry of a task's `check` list.
base::Result<Check> parse_check(const nlohmann::json& value);

struct CheckOutcome {
    bool ok { true };
    std::string detail;   // empty when ok; a human-readable reason otherwise
};

// Runs `checks` in order against `cwd`, like a `&&` chain: stops at the first failure (non-zero
// exit, a missing program, a timeout, or a content check that does not hold).
CheckOutcome run_checks(const std::vector<Check>& checks, const std::string& cwd, int timeoutSeconds);

// Copies a task's project, stripped of any build output a previous `mcpp build`/`mcpp test` left
// behind (target/, and the compile_commands.json mcpp writes at the project root) so every run
// starts from source only.
base::Result<void> copy_project(const std::string& projectDir, const std::string& dest);

// Copies every regular file under `source` to the same relative path under `destination`,
// creating directories as needed; an entry (file or directory) whose bare name makes `skip`
// return true is left out entirely, recursively for a directory. Built on mcpplibs.os `fs`
// rather than `std::filesystem::copy`/`copy_file` (measured: a no-op, without an error, under
// `mcpp test`'s sandboxing -- see tests/harness/tests/test_tasks.cpp).
base::Result<void> copy_tree(const std::string& source, const std::string& destination,
                             const std::function<bool(std::string_view name)>& skip = {});

struct ValidateOutcome {
    bool ok { false };
    std::string detail;
};

// Whether a task's data holds: its baseline fails at least one check and its reference-patched
// copy passes every check. Both copies are made under `scratchRoot` and removed again.
ValidateOutcome validate_task(const Task& task, const std::string& scratchRoot, int timeoutSeconds);

} // namespace mcppls::harness
