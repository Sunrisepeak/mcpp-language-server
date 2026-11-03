module mcppls.engine.clangd.grants;

import std;
import mcpplibs.os;
import mcppls.base.home;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.normalize.plan;

namespace os = mcpplibs::os;

namespace mcppls::engine::clangd {

namespace {

// The grants one start may carry: openkal-linux, -macos and -windows 0.23 refuse more, and the C library
// holds no more.
constexpr std::size_t MAX_GRANTS { 16 };

// A directory that is there and that this program can open: the implementation opens each grant to give it,
// and a start with one it cannot open is refused whole.
bool is_directory(const std::string& path) {
    std::error_code error;
    if (path.empty() || !std::filesystem::is_directory(path, error)) return false;
    std::filesystem::directory_iterator opened { path, error };
    return !error;
}

// A flag that names a directory to search or a file to read, joined to its value or before it.
struct PathFlag {
    std::string_view spelling;
    bool file;   // the value is a file: its directory is what is reached
};
constexpr std::array PATH_FLAGS {
    PathFlag { "-I", false },          PathFlag { "-isystem", false },   PathFlag { "-iquote", false },
    PathFlag { "-idirafter", false },  PathFlag { "-imsvc", false },     PathFlag { "/I", false },
    PathFlag { "-isysroot", false },   PathFlag { "--sysroot", false },  PathFlag { "-resource-dir", false },
    PathFlag { "-B", false },          PathFlag { "-include", true },    PathFlag { "-fprebuilt-module-path", false },
    PathFlag { "-fmodule-file", true }, PathFlag { "-iprefix", false },  PathFlag { "-cxx-isystem", false },
};

// The value of `argument` (and of the one after it, `next`) when it is `flag`'s: `-I/x`, `-I /x`,
// `--sysroot=/x`, `-fmodule-file=name=/x/m.pcm`; `took_next` says the next argument was the value.
std::optional<std::string_view> flag_value(std::string_view argument, std::string_view next, const PathFlag& flag,
                                           bool& took_next) {
    took_next = false;
    if (!argument.starts_with(flag.spelling)) return std::nullopt;
    std::string_view rest { argument.substr(flag.spelling.size()) };
    if (rest.empty()) {
        if (next.empty()) return std::nullopt;
        took_next = true;
        return next;
    }
    if (rest.front() == '=') rest.remove_prefix(1);
    // `--sysroot' and `-f...' take their value after `='; the others also joined (`-isystem/x'), and
    // a longer flag that begins with one of them (`-include-pch') is not it.
    else if (flag.spelling.starts_with("--") || flag.spelling.starts_with("-f") || rest.front() == '-') return std::nullopt;
    // -fmodule-file=<name>=<path>: the path after the last '='.
    if (flag.spelling == "-fmodule-file") {
        if (const std::size_t at { rest.rfind('=') }; at != std::string_view::npos) rest = rest.substr(at + 1);
    }
    return rest.empty() ? std::nullopt : std::optional<std::string_view> { rest };
}

// The longest directory both `a` and `b` are beneath, in this spelling; empty when they share none (two volumes).
std::string common_ancestor(std::string_view a, std::string_view b) {
    if (base::is_within(a, b)) return std::string { b };
    if (base::is_within(b, a)) return std::string { a };
    std::size_t end { 0 };   // just past the last separator both share
    for (std::size_t i { 0 }; i < a.size() && i < b.size(); ++i) {
        if (base::path_key(a.substr(i, 1)) != base::path_key(b.substr(i, 1))) break;
        if (a[i] == '/') end = i + 1;
    }
    if (end == 0) return {};
    std::string out { a.substr(0, end) };
    // "/" and "C:/" keep their separator; any other directory is named without it.
    if (out.size() > 1 && !(out.size() == 3 && out[1] == ':')) out.pop_back();
    return out;
}

// Of the read-only grants after the first, the directory two of them are beneath that is deepest;
// empty when no two share one.
std::string deepest_shared(std::span<const Grant> grants) {
    std::string best;
    for (std::size_t i { 1 }; i < grants.size(); ++i) {
        for (std::size_t j { i + 1 }; j < grants.size(); ++j) {
            if (grants[i].writable || grants[j].writable) continue;
            std::string ancestor { common_ancestor(grants[i].path, grants[j].path) };
            if (ancestor.empty() || ancestor.size() <= best.size()) continue;
            best = std::move(ancestor);
        }
    }
    return best;
}

void add_unique(std::vector<std::string>& out, std::string path) {
    if (path.empty()) return;
    path = base::normalize_path(path);
    if (std::ranges::none_of(out, [&](const std::string& known) { return base::same_path(known, path); })) out.push_back(std::move(path));
}

} // namespace

GrantSetting parse_grant_setting(std::span<const std::string> members) {
    GrantSetting setting;
    for (const auto& member : members) {
        const std::string_view text { base::trim(member) };
        if (text.empty()) continue;
        if (text == "all") {
            setting.all = true;
            continue;
        }
        Grant grant;
        std::string_view path { text };
        if (path.starts_with("rw:")) {
            grant.writable = true;
            path.remove_prefix(3);
        }
        if (!base::is_absolute_path(path)) {
            setting.problems.push_back(std::format("mcppls.grants: {} is not an absolute path (a path, or rw:<path>)", text));
            continue;
        }
        grant.path = base::normalize_path(path);
        setting.added.push_back(std::move(grant));
    }
    return setting;
}

std::vector<std::string> plan_directories(const normalize::EnginePlan& plan) {
    std::vector<std::string> out;
    for (const auto& entry : plan.entries) {
        const auto absolute = [&](std::string_view path) {
            return base::is_absolute_path(path) ? std::string { path } : base::join_path(entry.directory, path);
        };
        add_unique(out, entry.directory);
        if (!entry.file.empty()) add_unique(out, base::parent_path(absolute(entry.file)));
        // The compiler's installation: the directory above the one its program is in (<prefix>/bin/clang++),
        // where its headers and libraries are found. A bare name is found on PATH and adds nothing here.
        if (!entry.arguments.empty() && base::is_absolute_path(entry.arguments.front()))
            add_unique(out, base::parent_path(base::parent_path(entry.arguments.front())));
        for (std::size_t i { 1 }; i < entry.arguments.size(); ++i) {
            const std::string_view argument { entry.arguments[i] };
            const std::string_view next { i + 1 < entry.arguments.size() ? std::string_view { entry.arguments[i + 1] } : std::string_view {} };
            for (const auto& flag : PATH_FLAGS) {
                bool took_next { false };
                const auto value = flag_value(argument, next, flag, took_next);
                if (!value) continue;
                const std::string path { absolute(*value) };
                add_unique(out, flag.file ? base::parent_path(path) : path);
                if (took_next) ++i;
                break;
            }
        }
    }
    return out;
}

std::vector<Grant> engine_grants(const GrantInputs& inputs) {
    std::vector<Grant> candidates;
    const auto offer = [&](const std::string& path, bool writable, bool unchanging) {
        if (!is_directory(path)) return;
        const std::string spelled { base::normalize_path(path) };
        candidates.push_back(Grant { spelled, writable, unchanging });
        // A directory reached through a link is named by the engine both ways: the one it was told, and
        // the one the link resolves to, which a header's own includes are found beneath.
        if (const std::string resolved { base::canonical_path(spelled) }; !resolved.empty() && !base::same_path(resolved, spelled))
            candidates.push_back(Grant { resolved, writable, unchanging });
    };
    for (const auto& path : inputs.writable) offer(path, true, false);
    for (const auto& path : inputs.payload) offer(path, false, true);
    for (const auto& path : inputs.readable) offer(path, false, false);
    for (const auto& path : inputs.named) offer(path, false, false);
    for (const auto& grant : inputs.added) offer(grant.path, grant.writable, false);
    // Outermost first, so that one beneath a grant with as many rights is left out.
    std::ranges::stable_sort(candidates, std::less {}, [](const Grant& g) { return g.path.size(); });
    std::vector<Grant> grants;
    if (is_directory(inputs.root)) {
        const std::string root { base::normalize_path(inputs.root) };
        grants.push_back(Grant { root, true, false });
        if (const std::string resolved { base::canonical_path(root) }; !resolved.empty() && !base::same_path(resolved, root))
            grants.push_back(Grant { resolved, true, false });
    }
    for (auto& candidate : candidates) {
        const bool held = std::ranges::any_of(grants, [&](const Grant& g) {
            return base::is_within(candidate.path, g.path) && (g.writable || !candidate.writable);
        });
        if (!held) grants.push_back(std::move(candidate));
    }
    // AT MOST SIXTEEN, the number every implementation gives one start (openkal-linux, -macos and -windows 0.23
    // refuse more, and the C library holds no more). Two read-only grants become the directory both are beneath,
    // the pair whose directory is deepest first. A set that still does not fit is retained: the start must be
    // refused by the system, never replaced with the runner's unrestricted defaults.
    while (grants.size() > MAX_GRANTS) {
        const std::string best { deepest_shared(grants) };
        if (best.empty()) return grants;
        std::erase_if(grants, [&](const Grant& g) { return !g.writable && base::is_within(g.path, best); });
        // The wider directory also contains files outside the original grants;
        // immutable children cannot promise that their parent never changes.
        grants.push_back(Grant { best, false, false });
    }
    return grants;
}

bool covered(std::span<const Grant> grants, std::string_view directory) {
    return std::ranges::any_of(grants, [&](const Grant& g) { return base::is_within(directory, g.path); });
}

std::vector<std::string> uncovered(std::span<const Grant> grants, std::span<const std::string> named) {
    std::vector<std::string> out;
    for (const auto& directory : named) {
        if (!is_directory(directory) || covered(grants, directory)) continue;
        const std::string resolved { base::canonical_path(directory) };
        if (!resolved.empty() && covered(grants, resolved)) continue;
        out.push_back(directory);
    }
    return out;
}

std::vector<std::string> system_roots() {
    std::vector<std::string> roots;
    switch (os::host()) {
    case os::platform::windows: {
        for (const char* variable : { "ProgramFiles", "ProgramFiles(x86)", "ProgramData" }) {
            if (const auto value = os::env::get(variable); value && !value->empty()) roots.push_back(base::normalize_path(*value));
        }
        break;
    }
    case os::platform::macos:
        for (const char* root : { "/usr", "/opt", "/Library/Developer", "/Applications", "/System/Library/Frameworks",
                                  "/private/var/select", "/nix" })
            roots.emplace_back(root);
        break;
    default:
        for (const char* root : { "/usr", "/opt", "/lib", "/lib64", "/lib32", "/bin", "/etc", "/nix", "/gnu" })
            roots.emplace_back(root);
        break;
    }
    std::erase_if(roots, [](const std::string& root) { return !is_directory(root); });
    return roots;
}

std::vector<std::string> runner_caches() {
    std::vector<std::string> out;
    const auto made = [&](std::string path) {
        std::error_code ignored;
        std::filesystem::create_directories(path, ignored);
        if (is_directory(path)) out.push_back(base::normalize_path(path));
    };
    if (const auto set = os::env::get("OKRUN_CACHE"); set && !set->empty()) made(*set);
    else if (const auto runtime = os::env::get("XDG_RUNTIME_DIR"); runtime && !runtime->empty() && os::host() != os::platform::windows)
        made(base::join_path(*runtime, "okrun"));
    // The user's cache directory, as the runner places it: %LOCALAPPDATA% on Windows, ~/Library/Caches on
    // macOS, $XDG_CACHE_HOME or ~/.cache elsewhere.
    std::string cache;
    if (os::host() == os::platform::windows) {
        if (const auto local = os::env::get("LOCALAPPDATA"); local && !local->empty()) cache = *local;
    } else if (os::host() == os::platform::macos) {
        cache = base::join_path(base::home_directory(), "Library/Caches");
    } else if (const auto xdg = os::env::get("XDG_CACHE_HOME"); xdg && !xdg->empty()) {
        cache = *xdg;
    } else {
        cache = base::join_path(base::home_directory(), ".cache");
    }
    if (!cache.empty()) made(base::join_path(cache, "okrun"));
    return out;
}

} // namespace mcppls::engine::clangd
