// The directories an ok-image engine is given (mcpp design 2026-11-03 part 4 §9.3; openkal 0.23).
//
// clangd.okb reaches files only through the directories its runner was given, which okrun passes on
// to the image and the image to every module worker it starts. A runner given none has the whole file
// system. mcppls gives an ok-image engine what it reads and writes, and no more: the project, which it
// writes; its caches, the temporary directory and the runner's caches; the engine's own files; the
// package stores and the system's roots, which hold toolchains and their headers; and every directory
// the engine database names. A database that names a directory the engine was not given starts the
// engine again with it. `mcppls.grants` adds directories, or gives the runner's defaults (`all`).
export module mcppls.engine.clangd.grants;

import std;
import mcppls.normalize.plan;

export namespace mcppls::engine::clangd {

struct Grant {
    std::string path;            // absolute, as the engine names it
    bool writable { false };
    bool unchanging { false };   // nothing beneath changes while the engine runs: the engine's own files
    bool operator==(const Grant&) const = default;
};

// What `mcppls.grants` states: nothing (the derived set), `all` (the runner's own defaults, the whole
// file system), and directories added to the derived set: a path is read-only, `rw:<path>` writable.
struct GrantSetting {
    bool all { false };
    std::vector<Grant> added;
    std::vector<std::string> problems;   // members that are not an absolute path
};
GrantSetting parse_grant_setting(std::span<const std::string> members);

// The directories a plan's commands name: each command's directory and its source's, the compiler's
// installation (the directory above the one its program is in -- a bare name resolved by
// `resolve_program` when given), and each directory a flag searches or reads beneath (-I, -isystem,
// -fmodule-file=, -fprebuilt-module-path=), made absolute against the command's directory.
using ProgramResolver = std::function<std::optional<std::string>(std::string_view)>;
std::vector<std::string> plan_directories(const normalize::EnginePlan& plan, const ProgramResolver& resolve_program = {});

struct GrantInputs {
    std::string root;                      // the project: the directory the engine runs in, writable
    std::vector<std::string> writable;     // mcppls's caches, the temporary directory, the runner's caches
    std::vector<std::string> payload;      // the engine's own files, unchanging
    std::vector<std::string> readable;     // the package stores and the system's roots
    std::vector<std::string> named;        // what the engine database names (plan_directories)
    std::vector<Grant> added;              // mcppls.grants
};

// The grant set: the root first, then each directory that is there and not beneath another grant with
// as many rights, each also under the name its links resolve to where that differs.
std::vector<Grant> engine_grants(const GrantInputs& inputs);

// Whether `directory` is a grant or beneath one.
bool covered(std::span<const Grant> grants, std::string_view directory);

// What `named` holds that is there and that `grants` do not cover: why an engine is started again.
std::vector<std::string> uncovered(std::span<const Grant> grants, std::span<const std::string> named);

// The roots of this system's toolchains and headers that are there: /usr, /opt and the like (Linux,
// macOS), the developer directories (macOS). Windows names none (issue 86): the plan names what
// its toolchains need, and mcppls.grants adds what it does not.
std::vector<std::string> system_roots();

// Where the ok-image runner keeps what it prepares (openkal-image 0.4): OKRUN_CACHE, else
// $XDG_RUNTIME_DIR/okrun, and the user's cache directory's okrun, each made if absent.
std::vector<std::string> runner_caches();

} // namespace mcppls::engine::clangd
