module mcppls.devtools.check;

import std;
import mcpplibs.cmdline;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.platform.env;
import mcppls.devtools.common;
import mcppls.devtools.version;

namespace mcppls::devtools::check {
namespace fs = mcppls::platform::fs;
namespace {

constexpr std::array<std::string_view, 2> SOURCE_EXTENSIONS { ".cppm", ".cpp" };
constexpr std::array<std::string_view, 0> NO_SKIP {};

// ---- os-surface: ported from tools/check_os_surface.py -----------------------------------------

constexpr std::array<std::string_view, 6> SIX {
    "FAMILY", "FAMILY_NAME", "EXECUTABLE_SUFFIX", "PATH_LIST_SEPARATOR", "PLATFORM", "CASE_INSENSITIVE_PATHS",
};

// The NAME in a line shaped `inline constexpr <type> NAME {` -- the first all-caps token on the
// line that is immediately followed (after any spaces) by '{'. `Family` (the enum type) does not
// qualify because it contains lowercase letters; `FAMILY` (the constant) does. Mirrors
// check_os_surface.py's `DECL` regex without pulling in <regex> for one shape.
std::optional<std::string> declared_name(std::string_view line) {
    const std::string_view trimmed { base::trim(line) };
    if (!trimmed.starts_with("inline constexpr")) return std::nullopt;
    std::size_t i { 0 };
    while (i < trimmed.size()) {
        if (trimmed[i] >= 'A' && trimmed[i] <= 'Z') {
            const std::size_t start { i };
            std::size_t j { i + 1 };
            while (j < trimmed.size() &&
                   ((trimmed[j] >= 'A' && trimmed[j] <= 'Z') || (trimmed[j] >= '0' && trimmed[j] <= '9') || trimmed[j] == '_')) {
                ++j;
            }
            std::size_t k { j };
            while (k < trimmed.size() && (trimmed[k] == ' ' || trimmed[k] == '\t')) ++k;
            if (k < trimmed.size() && trimmed[k] == '{') return std::string { trimmed.substr(start, j - start) };
            i = j;
        } else {
            ++i;
        }
    }
    return std::nullopt;
}

std::string erase_six(std::string_view text) {
    std::string out;
    for (const auto line : base::split_lines(text)) {
        if (const auto name = declared_name(line); name && std::ranges::find(SIX, *name) != SIX.end()) {
            out += "<" + *name + ">\n";
        } else {
            out += std::string { line };
            out += "\n";
        }
    }
    return out;
}

// ---- layers / versions: a minimal TOML line scanner --------------------------------------------
// Enough to answer "what section is this key in, and is its value a bare quoted string, a
// `{ path = ... }` table, or a `.workspace = true` reference" -- everything layers/versions need,
// without a general TOML parser this repository has no other use for.

struct TomlLine {
    std::string section;
    std::string key;
    std::string value;
};

std::vector<TomlLine> scan_toml(std::string_view text) {
    std::vector<TomlLine> out;
    std::string section;
    for (const auto raw : base::split_lines(text)) {
        const std::string_view line { base::trim(raw) };
        if (line.empty() || line.starts_with('#')) continue;
        if (line.starts_with('[')) {
            section = std::string { line };
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        out.push_back({ section, std::string { base::trim(line.substr(0, eq)) }, std::string { base::trim(line.substr(eq + 1)) } });
    }
    return out;
}

std::string strip_quotes(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return std::string { value.substr(1, value.size() - 2) };
    return std::string { value };
}

bool is_local_path_dep(const TomlLine& line) { return line.value.starts_with('{') && line.value.contains("path"); }
bool is_pinned_third_party(const TomlLine& line) { return line.value.starts_with('"') && line.key != "path"; }

// The members `[workspace] members = [...]` in the root manifest declares, in the order written.
std::vector<std::string> workspace_members(std::string_view text) {
    std::vector<std::string> members;
    const auto section = text.find("[workspace]");
    if (section == std::string_view::npos) return members;
    const auto key = text.find("members", section);
    if (key == std::string_view::npos) return members;
    const auto open = text.find('[', key);
    const auto close = open == std::string_view::npos ? std::string_view::npos : text.find(']', open);
    if (open == std::string_view::npos || close == std::string_view::npos) return members;
    const std::string_view body { text.substr(open + 1, close - open - 1) };
    for (std::size_t i { 0 }; i < body.size();) {
        if (body[i] == '"') {
            const auto end = body.find('"', i + 1);
            if (end == std::string_view::npos) break;
            members.push_back(std::string { body.substr(i + 1, end - i - 1) });
            i = end + 1;
        } else {
            ++i;
        }
    }
    return members;
}

// Every path this check reasons about: the root package ("."), then each workspace member.
base::Result<std::vector<std::string>> all_member_paths(const std::string& root) {
    auto text = fs::read_file(base::join_path(root, "mcpp.toml"));
    if (!text) return std::unexpected { text.error() };
    std::vector<std::string> paths { "." };
    for (auto& member : workspace_members(*text)) paths.push_back(std::move(member));
    return paths;
}

std::string manifest_path_of(const std::string& root, const std::string& memberPath) {
    return memberPath == "." ? base::join_path(root, "mcpp.toml") : base::join_path(root, memberPath + "/mcpp.toml");
}

// Layer index by member PATH (tooling architecture §3.1's L0..L4): structural, not name-derived,
// because the check must still make sense if a package is ever renamed.
int layer_of_path(std::string_view path) {
    if (path == "modules/base") return 1;
    if (path == "modules/platform") return 2;
    if (path == "modules/pack") return 3;
    if (path.starts_with("modules/os/")) return 0;
    return 4;   // root "." and every tools/* member: the application layer
}

} // namespace

base::Result<Report> os_surface(const std::string& root) {
    const std::string osDir { base::join_path(root, "modules/os") };
    std::vector<std::string> paths;
    // `list_directory` already returns each child joined onto `osDir`; the file this looks for is
    // one level under that child, not under `osDir` again.
    for (const auto& child : fs::list_directory(osDir)) {
        const std::string candidate { base::join_path(child, "src/os.cppm") };
        if (fs::is_regular_file(candidate)) paths.push_back(candidate);
    }
    std::ranges::sort(paths);

    Report report {};
    if (paths.size() < 3) {
        report.ok = false;
        report.problems.push_back(std::format("{}/*/src/os.cppm matched {} file(s); expected at least three (linux, macos, windows)",
                                              osDir, paths.size()));
        return report;
    }

    std::vector<std::string> texts;
    for (const auto& path : paths) {
        auto text = fs::read_file(path);
        if (!text) return std::unexpected { text.error() };
        texts.push_back(*text);
    }

    for (std::size_t fi { 0 }; fi < paths.size(); ++fi) {
        std::vector<std::string> declared;
        for (const auto line : base::split_lines(texts[fi])) {
            if (auto name = declared_name(line)) declared.push_back(*name);
        }
        for (const auto& name : declared) {
            if (std::ranges::find(SIX, name) == SIX.end()) {
                report.ok = false;
                report.problems.push_back(std::format(
                    "{} declares `{}`, which is not one of the six platform constants -- a fact every target agrees on is not a platform fact",
                    paths[fi], name));
            }
        }
        for (const auto six : SIX) {
            const auto count = std::ranges::count(declared, six);
            if (count == 0) {
                report.ok = false;
                report.problems.push_back(std::format("{} does not declare `{}`", paths[fi], six));
            } else if (count > 1) {
                report.ok = false;
                report.problems.push_back(std::format("{} declares `{}` more than once", paths[fi], six));
            }
        }
    }

    const std::string reference { erase_six(texts[0]) };
    for (std::size_t i { 1 }; i < texts.size(); ++i) {
        if (erase_six(texts[i]) != reference) {
            report.ok = false;
            report.problems.push_back(std::format("{} and {} differ outside the six platform constants -- every such "
                                                  "difference is a platform branch this program did not have",
                                                  paths[0], paths[i]));
        }
    }

    if (report.ok) {
        report.notes.push_back(std::format("OK: {} os modules, {} platform constants, no other difference", paths.size(), SIX.size()));
    }
    return report;
}

base::Result<Report> layers(const std::string& root) {
    auto paths = all_member_paths(root);
    if (!paths) return std::unexpected { paths.error() };

    struct MemberInfo {
        std::string path;
        std::string name;
        std::vector<std::string> pathDeps;
    };
    std::vector<MemberInfo> infos;
    std::map<std::string, int> layerOf {
        { "openkal-llvm-runtime", 0 }, { "mcppls-os-linux", 0 }, { "mcppls-os-macos", 0 }, { "mcppls-os-windows", 0 },
    };

    for (const auto& path : *paths) {
        auto text = fs::read_file(manifest_path_of(root, path));
        if (!text) return std::unexpected { text.error() };
        const auto lines = scan_toml(*text);

        MemberInfo info {};
        info.path = path;
        for (const auto& line : lines) {
            if (line.section == "[package]" && line.key == "name") info.name = strip_quotes(line.value);
        }
        for (const auto& line : lines) {
            if (!line.section.contains("dependencies")) continue;
            if (is_local_path_dep(line)) info.pathDeps.push_back(line.key);
        }
        layerOf[info.name] = layer_of_path(path);
        infos.push_back(std::move(info));
    }

    Report report {};
    for (const auto& info : infos) {
        const int myLayer { layer_of_path(info.path) };
        const std::string displayPath { info.path == "." ? std::string { "the root package" } : info.path };
        for (const auto& dep : info.pathDeps) {
            if (dep == "mcppls-testing") continue;   // a dev-only harness, not part of the product layering
            if (dep == "mcppls-pack" && info.path != "tools/devtools") {
                report.ok = false;
                report.problems.push_back(std::format(
                    "{} depends on mcppls-pack, which only tools/devtools may depend on (§3.1: it carries tinyhttps "
                    "and libarchive, and no product may link either)",
                    displayPath));
                continue;
            }
            const auto found = layerOf.find(dep);
            if (found == layerOf.end()) {
                report.ok = false;
                report.problems.push_back(std::format("{} depends on {}, which devtools check layers does not know the layer of", displayPath, dep));
                continue;
            }
            if (found->second >= myLayer) {
                report.ok = false;
                report.problems.push_back(std::format("{} (layer {}) depends on {} (layer {}), which is not strictly lower",
                                                      displayPath, myLayer, dep, found->second));
            }
        }
    }

    // Only modules/platform, the three os packages, and modules/base/src/log.cpp (the one named
    // exception in §3.1) may `import openkal.*`.
    for (const auto& info : infos) {
        const std::string srcDir { info.path == "." ? base::join_path(root, "src") : base::join_path(root, info.path + "/src") };
        if (!fs::is_directory(srcDir)) continue;
        const bool wholeMemberAllowed { info.path == "modules/platform" };
        for (const auto& file : fs::list_files(srcDir, SOURCE_EXTENSIONS, NO_SKIP)) {
            const bool isBaseLogException { info.path == "modules/base" && file.ends_with("/log.cpp") };
            if (wholeMemberAllowed || isBaseLogException) continue;
            auto text = fs::read_file(file);
            if (!text) return std::unexpected { text.error() };
            std::size_t lineNumber { 0 };
            for (const auto line : base::split_lines(*text)) {
                ++lineNumber;
                if (base::trim(line).starts_with("import openkal.")) {
                    report.ok = false;
                    report.problems.push_back(std::format(
                        "{}:{} imports openkal.*, but only modules/platform and modules/base/src/log.cpp may (§3.1)",
                        file, lineNumber));
                }
            }
        }
    }

    if (report.ok) {
        report.notes.push_back("layers: every member depends only on strictly lower layers; mcppls-pack is reachable "
                               "from no product; openkal.* is imported from nowhere else but modules/platform and "
                               "modules/base's one exception");
    }
    return report;
}

base::Result<Report> versions(const std::string& root) {
    Report report {};
    auto problems = mcppls::devtools::version::check(root);
    if (!problems) return std::unexpected { problems.error() };
    if (!problems->empty()) {
        report.ok = false;
        for (auto& problem : *problems) report.problems.push_back(std::move(problem));
    }

    auto paths = all_member_paths(root);
    if (!paths) return std::unexpected { paths.error() };
    for (const auto& path : *paths) {
        auto text = fs::read_file(manifest_path_of(root, path));
        if (!text) return std::unexpected { text.error() };
        for (const auto& line : scan_toml(*text)) {
            if (!line.section.contains("dependencies")) continue;
            // `[workspace.dependencies]` (and its subsections) is where the ONE literal version of
            // each third-party package is declared -- that is the entire point of a workspace
            // dependency, and every member's own `[dependencies]` referring to it with
            // `.workspace = true` is what this check is actually guarding.
            if (line.section.starts_with("[workspace")) continue;
            if (is_pinned_third_party(line)) {
                report.ok = false;
                report.problems.push_back(std::format("{}/mcpp.toml: `{} = {}` is a pinned version; use `{}.workspace = true`",
                                                      path, line.key, line.value, line.key));
            }
        }
    }

    if (report.ok) {
        report.notes.push_back("versions: every derived site agrees with mcpp.toml, and every third-party dependency uses `.workspace = true`");
    }
    return report;
}

base::Result<Report> scripts(const std::string& root) {
    const std::string allowPath { base::join_path(root, "tools/devtools/scripts.allow") };
    auto allowText = fs::read_file(allowPath);
    if (!allowText) return std::unexpected { allowText.error() };
    std::set<std::string> allowed;
    std::vector<std::string> unexplained;
    for (const auto raw : base::split_lines(*allowText)) {
        const std::string_view line { base::trim(raw) };
        if (line.empty() || line.starts_with('#')) continue;
        const auto tab = line.find('\t');
        const std::string path { tab == std::string_view::npos ? line : line.substr(0, tab) };
        if (tab == std::string_view::npos || base::trim(line.substr(tab + 1)).empty()) unexplained.push_back(path);
        allowed.insert(path);
    }

    auto git = mcppls::platform::env::find_executable("git");
    if (!git) return base::fail("check-scripts", "git is not on PATH, so the tracked-scripts list cannot be read");
    auto output = capture(*git, { "ls-files", "--", "*.py", "*.sh" }, root);
    if (!output) return std::unexpected { output.error() };

    std::vector<std::string> tracked;
    for (const auto line : base::split_lines(*output)) {
        if (!line.empty()) tracked.push_back(std::string { line });
    }
    std::ranges::sort(tracked);

    Report report {};
    for (const auto& path : tracked) {
        if (!allowed.contains(path)) {
            report.ok = false;
            report.problems.push_back(std::format("{} is tracked but not in tools/devtools/scripts.allow -- add it with a reason, or remove the script", path));
        }
    }
    // The list is the record of every exception and why it stands (docs/93-devtools.md "What is not
    // C++"), so an entry for a script that is gone is a record that is wrong, not a harmless leftover.
    for (const auto& path : allowed) {
        if (!std::ranges::binary_search(tracked, path)) {
            report.ok = false;
            report.problems.push_back(std::format("tools/devtools/scripts.allow lists {}, which is no longer tracked -- remove the entry", path));
        }
    }
    for (const auto& path : unexplained) {
        report.ok = false;
        report.problems.push_back(std::format("tools/devtools/scripts.allow lists {} without a reason -- add one after a tab", path));
    }
    if (report.ok) {
        report.notes.push_back(std::format("scripts: every tracked *.py/*.sh ({} of them) is in tools/devtools/scripts.allow", tracked.size()));
    }
    return report;
}

namespace {

// ---- platforms: one table, held to by everything that cannot read it -------------------------

// The quoted strings on the line that declares SUPPORTED_PLATFORMS in the extension's source.
std::optional<std::vector<std::string>> typescript_platforms(std::string_view text) {
    for (const auto line : base::split_lines(text)) {
        if (!line.contains("SUPPORTED_PLATFORMS") || !line.contains('[')) continue;
        std::vector<std::string> names;
        const std::string_view list { line.substr(line.find('[')) };
        for (std::size_t i { 0 }; i < list.size(); ++i) {
            if (list[i] != '\'' && list[i] != '"') continue;
            const auto end = list.find(list[i], i + 1);
            if (end == std::string_view::npos) break;
            names.push_back(std::string { list.substr(i + 1, end - i - 1) });
            i = end;
        }
        return names;
    }
    return std::nullopt;
}

// The text of job `job` in a workflow file: from its `  <job>:` line to the next line at the same
// two-space indent. Enough YAML for "which platforms does this matrix list", without a YAML parser
// this repository has no other use for.
std::optional<std::string> workflow_job(std::string_view text, std::string_view job) {
    std::string out;
    bool inside { false };
    for (const auto line : base::split_lines(text)) {
        const bool topLevelKey { line.size() > 2 && line.starts_with("  ") && line[2] != ' ' && line[2] != '#' };
        if (inside && topLevelKey) break;
        if (!inside && topLevelKey && line.substr(2) == std::format("{}:", job)) {
            inside = true;
            continue;
        }
        if (inside) {
            out += line;
            out += '\n';
        }
    }
    if (!inside) return std::nullopt;
    return out;
}

// Every value written as `<key>: <value>` in `text`, whether in a flow mapping (`{ platform: x, ...}`)
// or a block one (`- platform: x`).
std::set<std::string> yaml_values(std::string_view text, std::string_view key) {
    std::set<std::string> values;
    const std::string needle { std::format("{}:", key) };
    for (std::size_t at { text.find(needle) }; at != std::string_view::npos; at = text.find(needle, at + 1)) {
        if (at > 0) {
            const char before { text[at - 1] };
            if (before != ' ' && before != '{' && before != ',' && before != '-') continue;
        }
        std::size_t i { at + needle.size() };
        while (i < text.size() && text[i] == ' ') ++i;
        std::size_t j { i };
        while (j < text.size() && text[j] != ',' && text[j] != '}' && text[j] != ' ' && text[j] != '\n') ++j;
        std::string value { text.substr(i, j - i) };
        if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"')) value = value.substr(1, value.size() - 2);
        if (!value.empty() && !value.starts_with("${{")) values.insert(value);
    }
    return values;
}

} // namespace

base::Result<Report> platforms(const std::string& root) {
    Report report {};
    const std::string lockPath { base::join_path(root, "packaging/payload.lock.json") };
    auto lockText = fs::read_file(lockPath);
    if (!lockText) return std::unexpected { lockText.error() };
    nlohmann::json lock;
    try {
        lock = nlohmann::json::parse(*lockText);
    } catch (const std::exception& error) {
        return base::fail("check-platforms", std::format("{} is not valid JSON: {}", lockPath, error.what()));
    }
    std::set<std::string> known;
    std::set<std::string> crossTargets;
    const auto entries = lock.value("entries", nlohmann::json::object());
    for (const auto& item : lock.value("platforms", nlohmann::json::object()).items()) {
        const std::string name { item.key() };
        const auto& row = item.value();
        known.insert(name);
        const auto dash = name.find('-');
        const std::string os { dash == std::string::npos ? std::string {} : name.substr(0, dash) };
        const std::string arch { dash == std::string::npos ? std::string {} : name.substr(dash + 1) };
        if ((os != "linux" && os != "darwin" && os != "win32") || (arch != "x64" && arch != "arm64")) {
            report.problems.push_back(std::format("{}: platform {} is not <os>-<arch> (linux|darwin|win32 - x64|arm64)", lockPath, name));
        }
        const std::string clangd { row.value("clangd", std::string {}) };
        if (clangd.empty() || !entries.contains(clangd)) {
            report.problems.push_back(std::format("{}: platform {} names clangd entry \"{}\", which entries does not have", lockPath, name, clangd));
        }
        const std::string serverTarget { row.value("server-target", std::string {}) };
        if (serverTarget.empty()) {
            report.problems.push_back(std::format("{}: platform {} has no server-target", lockPath, name));
        } else if (name != "linux-x64") {
            // Every server but the Linux x64 host's own is cross-built by CI's cross-build job.
            crossTargets.insert(serverTarget);
        }
    }
    for (const auto& item : entries.items()) {
        const std::string name { item.key() };
        const auto& entry = item.value();
        if (!entry.contains("license-from")) continue;
        const std::string from { entry["license-from"].value("entry", std::string {}) };
        if (!entries.contains(from)) {
            report.problems.push_back(std::format("{}: entry {} takes its license from \"{}\", which entries does not have", lockPath, name, from));
        }
    }

    auto compare = [&](std::string_view where, const std::set<std::string>& listed) {
        for (const auto& name : known) {
            if (!listed.contains(name)) report.problems.push_back(std::format("{} does not list platform {}", where, name));
        }
        for (const auto& name : listed) {
            if (!known.contains(name)) report.problems.push_back(std::format("{} lists {}, which packaging/payload.lock.json has no platform for", where, name));
        }
    };

    const std::string tsPath { base::join_path(root, "editors/vscode/src/payload.ts") };
    auto tsText = fs::read_file(tsPath);
    if (!tsText) return std::unexpected { tsText.error() };
    if (auto listed = typescript_platforms(*tsText)) {
        compare(std::format("{} SUPPORTED_PLATFORMS", tsPath), { listed->begin(), listed->end() });
    } else {
        report.problems.push_back(std::format("{} declares no SUPPORTED_PLATFORMS", tsPath));
    }

    const std::string manifestPath { base::join_path(root, "packaging/release.manifest.json") };
    auto manifestText = fs::read_file(manifestPath);
    if (!manifestText) return std::unexpected { manifestText.error() };
    try {
        const auto manifest = nlohmann::json::parse(*manifestText);
        std::set<std::string> listed;
        for (const auto& name : manifest.value("platforms", nlohmann::json::array())) listed.insert(name.get<std::string>());
        compare(std::format("{} platforms", manifestPath), listed);
    } catch (const std::exception& error) {
        return base::fail("check-platforms", std::format("{} is not valid JSON: {}", manifestPath, error.what()));
    }

    // The jobs that run once per platform. A platform left out of one of them ships untested or
    // unbuilt; one listed there and not in the lock builds a payload nothing can assemble.
    struct Job {
        std::string_view file;
        std::string_view job;
    };
    for (const Job job : { Job { "ci.yml", "payload" }, Job { "ci.yml", "conformance" }, Job { "ci.yml", "vscode-e2e" },
                           Job { "release-checks.yml", "install" } }) {
        const std::string path { base::join_path(root, std::format(".github/workflows/{}", job.file)) };
        auto text = fs::read_file(path);
        if (!text) return std::unexpected { text.error() };
        const auto block = workflow_job(*text, job.job);
        if (!block) {
            report.problems.push_back(std::format("{} has no job {}", path, job.job));
            continue;
        }
        compare(std::format("{} job {}", path, job.job), yaml_values(*block, "platform"));
    }
    {
        const std::string path { base::join_path(root, ".github/workflows/ci.yml") };
        auto text = fs::read_file(path);
        if (!text) return std::unexpected { text.error() };
        if (const auto block = workflow_job(*text, "cross-build")) {
            const auto listed = yaml_values(*block, "target");
            for (const auto& target : crossTargets) {
                if (!listed.contains(target)) report.problems.push_back(std::format("{} job cross-build does not build {}", path, target));
            }
        } else {
            report.problems.push_back(std::format("{} has no job cross-build", path));
        }
    }

    report.ok = report.problems.empty();
    if (report.ok) {
        report.notes.push_back(std::format("platforms: {} in the lock, and the extension, the release manifest and CI's per-platform jobs list exactly those",
                                           base::join(std::vector<std::string> { known.begin(), known.end() }, ", ")));
    }
    return report;
}

base::Result<Report> binary(const std::string& serverPath) {
    if (!fs::is_regular_file(serverPath)) {
        return base::fail("check-binary", std::format("{} is not a file", serverPath));
    }
    auto content = fs::read_file(serverPath);
    if (!content) return std::unexpected { content.error() };

    constexpr std::array<std::string_view, 2> FORBIDDEN { "archive_read_", "mbedtls_" };
    Report report {};
    for (const auto symbol : FORBIDDEN) {
        if (content->find(symbol) != std::string::npos) {
            report.ok = false;
            report.problems.push_back(std::format(
                "{} contains the symbol prefix `{}` -- a product must not link libarchive or mbedTLS (F1)", serverPath, symbol));
        }
    }
    if (report.ok) {
        report.notes.push_back(std::format("binary: {} carries no archive or TLS symbol", serverPath));
    }
    return report;
}

std::vector<std::string> commands_in_help(std::string_view help) {
    std::vector<std::string> commands;
    bool inside { false };
    for (const auto line : base::split_lines(help)) {
        if (base::trim(line) == "SUBCOMMANDS:") {
            inside = true;
            continue;
        }
        if (!inside) continue;
        if (line.empty() || !line.starts_with(' ')) break;
        // A command is indented by four; a description's own continuation lines are indented less.
        if (!line.starts_with("    ") || line.size() < 5 || line[4] == ' ') continue;
        const std::string_view entry { base::trim(line) };
        commands.emplace_back(entry.substr(0, entry.find(' ')));
    }
    return commands;
}

base::Result<Report> docs(const std::string& root, const std::vector<std::string>& commands) {
    const std::string path { base::join_path(root, "docs/93-devtools.md") };
    auto text = fs::read_file(path);
    if (!text) return std::unexpected { text.error() };

    // A command is named as `-p devtools -- <name>` or `mcppls-devtools <name>`.
    std::set<std::string> named;
    for (const std::string_view marker : { std::string_view { "-p devtools -- " }, std::string_view { "mcppls-devtools " } }) {
        for (std::size_t at { text->find(marker) }; at != std::string::npos; at = text->find(marker, at + 1)) {
            std::size_t begin { at + marker.size() }, end { begin };
            while (end < text->size() && (std::islower(static_cast<unsigned char>((*text)[end])) || (*text)[end] == '-')) ++end;
            if (end > begin) named.emplace(text->substr(begin, end - begin));
        }
    }

    Report report {};
    for (const auto& command : commands) {
        if (!named.contains(command)) {
            report.ok = false;
            report.problems.push_back(std::format("docs/93-devtools.md does not mention `{}` -- add it to the table of its task", command));
        }
    }
    for (const auto& name : named) {
        if (std::ranges::find(commands, name) == commands.end()) {
            report.ok = false;
            report.problems.push_back(std::format("docs/93-devtools.md names `{}`, which mcppls-devtools does not have", name));
        }
    }
    if (report.ok) {
        report.notes.push_back(std::format("docs: docs/93-devtools.md names all {} commands and no other", commands.size()));
    }
    return report;
}

} // namespace mcppls::devtools::check

namespace mcppls::devtools {
namespace {

namespace cmdline = mcpplibs::cmdline;

void print_report(const check::Report& report, bool json, std::string_view name) {
    if (json) {
        nlohmann::json value;
        value["check"] = name;
        value["ok"] = report.ok;
        value["problems"] = report.problems;
        value["notes"] = report.notes;
        print_json(value);
        return;
    }
    for (const auto& note : report.notes) std::println("{}", note);
    for (const auto& problem : report.problems) std::println(std::cerr, "FAIL: {}", problem);
}

int run_check(base::Result<check::Report> result, bool json, std::string_view name) {
    if (!result) {
        std::println(std::cerr, "mcppls-devtools: {}", result.error().message);
        return 1;
    }
    print_report(*result, json, name);
    return result->ok ? 0 : 1;
}

// This program's own commands, as its --help lists them: what the docs check compares against,
// so a command added without documentation is caught by the same run that builds it.
base::Result<check::Report> docs_of_this_program(const std::string& root) {
    const auto self = mcppls::platform::env::arguments();
    std::string program { base::normalize_path(self.empty() ? std::string {} : self.front()) };
    if (!base::is_absolute_path(program)) program = base::join_path(mcppls::platform::fs::current_directory(), program);
    auto help = capture(program, { "--help" }, root, std::chrono::seconds { 30 });
    if (!help) return std::unexpected { help.error() };
    return check::docs(root, check::commands_in_help(*help));
}

int command_check_all(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };
    const bool json { arguments.is_flag_set("json") };

    struct Named {
        std::string_view name;
        base::Result<check::Report> report;
    };
    std::vector<Named> results {
        { "os-surface", check::os_surface(root) },
        { "layers", check::layers(root) },
        { "versions", check::versions(root) },
        { "scripts", check::scripts(root) },
        { "platforms", check::platforms(root) },
        { "docs", docs_of_this_program(root) },
    };

    bool ok { true };
    if (json) {
        auto value = nlohmann::json::array();
        for (const auto& named : results) {
            nlohmann::json entry;
            entry["check"] = named.name;
            if (!named.report) {
                entry["error"] = named.report.error().message;
                ok = false;
            } else {
                entry["ok"] = named.report->ok;
                entry["problems"] = named.report->problems;
                entry["notes"] = named.report->notes;
                ok = ok && named.report->ok;
            }
            value.push_back(std::move(entry));
        }
        print_json(value);
        return ok ? 0 : 1;
    }
    for (const auto& named : results) {
        std::println("-- {} --", named.name);
        if (!named.report) {
            std::println(std::cerr, "mcppls-devtools: {}", named.report.error().message);
            ok = false;
            continue;
        }
        print_report(*named.report, false, named.name);
        ok = ok && named.report->ok;
    }
    return ok ? 0 : 1;
}

// `cmdline::App::run` only dispatches one level of subcommand: given `devtools check os-surface`,
// the top-level App finds and calls `check`'s own action, but does not then recurse into `check`'s
// matching child to call ITS action -- there is no second `.run()` call anywhere in the library for
// that. So `check`'s own action, below, does that second level of dispatch by hand rather than
// relying on the per-verb `.action(...)` a naive reading of the builder API would reach for (and
// which would simply never run). `release_command` and `measure_command` carry the same shape for
// the same reason.
int dispatch(bool& handled, int& status, std::string_view verb, const cmdline::ParsedArgs& inner) {
    handled = true;
    const std::string root { repository_root() };
    const bool json { inner.is_flag_set("json") };
    if (verb == "os-surface") return run_check(check::os_surface(root), json, "os-surface");
    if (verb == "layers") return run_check(check::layers(root), json, "layers");
    if (verb == "versions") return run_check(check::versions(root), json, "versions");
    if (verb == "scripts") return run_check(check::scripts(root), json, "scripts");
    if (verb == "platforms") return run_check(check::platforms(root), json, "platforms");
    if (verb == "docs") return run_check(docs_of_this_program(root), json, "docs");
    if (verb == "binary") {
        const auto server = inner.value("server");
        if (!server) {
            std::println(std::cerr, "mcppls-devtools: check binary needs --server PATH");
            return 2;
        }
        return run_check(check::binary(*server), json, "binary");
    }
    if (verb == "all") return command_check_all(inner);
    std::println(std::cerr, "mcppls-devtools: check needs a verb: os-surface, layers, versions, scripts, platforms, docs, binary, all");
    return 2;
}

} // namespace

cmdline::App check_command(bool& handled, int& status) {
    cmdline::App command { "check" };
    (void) command.description("Repository invariants: platform surface, layering, versions, scripts, platforms, the built binary");

    (void) command.subcommand("os-surface")
        .description("The platform surface is six constants (ported from tools/check_os_surface.py)")
        .option("json").help("Structured output");

    (void) command.subcommand("layers")
        .description("Every member depends only on lower layers; mcppls-pack is reachable from no product (§3.1)")
        .option("json").help("Structured output");

    (void) command.subcommand("versions")
        .description("version --check, plus every third-party dependency uses `.workspace = true`")
        .option("json").help("Structured output");

    (void) command.subcommand("scripts")
        .description("Every tracked *.py/*.sh is declared in tools/devtools/scripts.allow")
        .option("json").help("Structured output");

    (void) command.subcommand("platforms")
        .description("The extension, the release manifest and CI's per-platform jobs list exactly the lock's platforms")
        .option("json").help("Structured output");

    (void) command.subcommand("docs")
        .description("docs/93-devtools.md names every command this program has, and no other")
        .option("json").help("Structured output");

    (void) command.subcommand("binary")
        .description("The built server's symbol table carries no archive or TLS symbol (F1's backstop)")
        .option("server").takes_value().value_name("PATH").help("The server binary to inspect").option("json")
        .help("Structured output");

    (void) command.subcommand("all")
        .description("Every source-only check above (not `binary`, which needs a built server)")
        .option("json").help("Structured output");

    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) {
        const auto sub = arguments.subcommand();
        static const cmdline::ParsedArgs EMPTY {};
        status = dispatch(handled, status, arguments.subcommand_name(), sub ? sub->get() : EMPTY);
    });
    return command;
}

} // namespace mcppls::devtools
