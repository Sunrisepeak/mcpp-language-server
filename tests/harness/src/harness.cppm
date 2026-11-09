// How a case drives the server (tests/cts/README.md): `run` starts `mcppls-conformance run` on a
// fixture of tests/cts/fixtures as a client of the server, and every check the runner reports
// failed is a failure of the case, named by its id. What the runner measured is returned, for a
// case that reports it (tests/bench).
//
// The environment names what the cases run against; CI sets it:
//   MCPPLS_CTS_SERVER    the server under test            (default payload/bin/mcppls.okb)
//   MCPPLS_CTS_PAYLOAD   its payload                      (default payload)
//   MCPPLS_CTS_RUNNER    mcppls-conformance               (default: beside this program)
//   MCPPLS_CTS_FIXTURES  the fixtures                     (default tests/cts/fixtures)
//   MCPPLS_CTS_TIMEOUT   seconds each check may take      (default 180)
//   MCPPLS_CTS_MSVC_ENV  a developer environment (NAME=value lines) for the fixtures that build with MSVC
//   MCPPLS_CTS_MEASURES  a directory each case's measures are written to
//   MCPPLS_CTS_BUNDLES   a directory the diagnostic bundles the bundle checks exported are left in
//   MCPPLS_CTS_EVIDENCE  a directory a session that keeps evidence keeps its workspace and cache in
//   MCPPLS_CTS_VERBOSE   1: the runner prints the server's log (a failed case's diagnosis sets it)
export module mcppls.harness;

import std;
import nlohmann.json;
import mcpplibs.os;
import mcpplibs.mtest;
import mcpplibs.mtest.e2e;

export namespace mcppls::harness {

// The platforms a case runs on, as tags `on:<platform>` a CI row selects by (`mtest run --tag`).
struct on {
    std::vector<std::string_view> platforms;
    on(std::initializer_list<std::string_view> names) : platforms(names) {}
};

void apply(mcpplibs::mtest::attributes& a, on o) {
    for (const auto p : o.platforms)
        a.tags.push_back(std::format("on:{}", p));
}

// The client the runner is: its own, or one that declares no dynamic registration of watched
// files (polling), no `experimental.cxxModules` (plain), or what an editor sends.
enum class client { own, polling, plain, vscode, neovim, zed };

// The client a variant names (`fixture@variant`); its own for any other.
client client_of(std::string_view variant);

// One session of the runner on a fixture.
struct session {
    std::string fixture;           // a directory of the fixtures
    client as = client::own;
    std::string stage;             // the fixture's stage of that name alone
    bool warm = false;             // the cache a session before this one left is reused (--expect-warm)
    std::filesystem::path home;    // the workspace and cache of a sequence of sessions; empty: the runner's own
    std::string cache = "cache";   // the cache's directory under `home`: sessions on another start cold
    bool linked = false;           // the runner's temporary directory, its workspace's, is reached through a symbolic link
    bool evidence = false;         // its workspace and cache are kept for the record (MCPPLS_CTS_EVIDENCE)
    int timeout = 0;               // seconds each check may take; 0: MCPPLS_CTS_TIMEOUT
    std::vector<std::string> arguments;  // the runner's further arguments
};

// Runs the session, judges it, and returns what the runner measured (null when it wrote nothing).
nlohmann::json run(const session& s);

// The value of environment variable `name`, or `fallback` when it is unset or empty.
std::string env_or(const char* name, std::string_view fallback);

}  // namespace mcppls::harness

namespace mcppls::harness {

namespace os = mcpplibs::os;
namespace mt = mcpplibs::mtest;
using json = nlohmann::json;

std::string env_or(const char* name, std::string_view fallback) {
    auto v = os::env::get(name);
    return v && !v->empty() ? *v : std::string(fallback);
}

client client_of(std::string_view variant) {
    if (variant == "polling") return client::polling;
    if (variant == "plain") return client::plain;
    if (variant == "vscode") return client::vscode;
    if (variant == "neovim") return client::neovim;
    if (variant == "zed") return client::zed;
    return client::own;
}

namespace {

std::string runner() {
    if (auto v = os::env::get("MCPPLS_CTS_RUNNER"); v && !v->empty())
        return *v;
    const auto self = mt::self_command();
    const std::string exe = os::host() == os::platform::windows ? ".exe" : "";
    if (!self.empty()) {
        const auto beside = os::fs::from_utf8(self.back()).parent_path() / ("mcppls-conformance" + exe);
        if (std::filesystem::exists(beside))
            return os::fs::utf8(beside);
    }
    return mt::e2e::program("mcppls-conformance");
}

std::string file_part(std::string_view id) {
    std::string out;
    for (const char c : id)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '-';
    return out;
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    for (const auto part : std::views::split(text, '\n')) {
        std::string line(part.begin(), part.end());
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

// The name of a session's measure file: its case's, then its stage or its being warm, numbered when
// its case has used the name before (the rounds of a measurement).
std::string measure_name(const std::string& id, const session& s) {
    auto name = file_part(id);
    if (!s.stage.empty() && !id.ends_with("/" + s.stage))
        name += "-" + s.stage;
    else if (s.stage.empty() && s.warm)
        name += "-warm";
    static std::map<std::string, int> used;
    if (const int n = ++used[name]; n > 1)
        name += std::format("-{}", n);
    return name;
}

// The failures a run reports: each failed check from its measures, then each other `FAIL` line
// (the handshake, a requirement, a budget of the whole run).
void judge(const mt::e2e::outcome& r, const json& measured, std::string_view fixture) {
    using mt::expect;
    expect(mt::fatal(r.started)) << std::format("mcppls-conformance did not start: {}", r.err);
    std::set<std::string> named;
    for (const auto& c : measured.is_object() ? measured.value("checks", json::array()) : json::array()) {
        if (c.value("ok", true) || c.value("optional", false))
            continue;
        named.insert(c.value("id", ""));
        expect(false) << std::format("{} {} {}: {}", fixture, c.value("id", ""), c.value("kind", ""),
                                     c.value("detail", ""));
    }
    bool any = !named.empty();
    for (const auto& line : lines_of(r.out)) {
        if (!line.starts_with("FAIL "))
            continue;
        const auto rest = std::string_view(line).substr(5);
        if (named.contains(std::string(rest.substr(0, rest.find(' ')))))
            continue;
        any = true;
        expect(false) << std::format("{} {}", fixture, rest);
    }
    if (r.timed_out)
        expect(false) << "mcppls-conformance was still running at its limit";
    else if (r.status != 0 && !any)
        expect(false) << std::format("mcppls-conformance exited {} and named no failed check", r.status);
    if (r.status != 0 || r.timed_out)
        std::print("{}{}", r.out, r.err);
}

}  // namespace

json run(const session& s) {
    const auto id = mt::current_case();
    std::vector<std::string> argv{runner(), "run",
                                  "--server", env_or("MCPPLS_CTS_SERVER", "payload/bin/mcppls.okb"),
                                  "--payload", env_or("MCPPLS_CTS_PAYLOAD", "payload"),
                                  "--fixture", os::fs::utf8(os::fs::from_utf8(env_or("MCPPLS_CTS_FIXTURES", "tests/cts/fixtures")) / s.fixture),
                                  "--timeout", s.timeout > 0 ? std::to_string(s.timeout) : env_or("MCPPLS_CTS_TIMEOUT", "180")};
    switch (s.as) {
        case client::own: break;
        case client::polling: argv.push_back("--no-dynamic-watch"); break;
        case client::plain: argv.insert(argv.end(), {"--client", "plain"}); break;
        case client::vscode: argv.insert(argv.end(), {"--client", "vscode"}); break;
        case client::neovim: argv.insert(argv.end(), {"--client", "neovim"}); break;
        case client::zed: argv.insert(argv.end(), {"--client", "zed"}); break;
    }
    if (!s.stage.empty())
        argv.insert(argv.end(), {"--stage", s.stage});
    if (s.warm)
        argv.push_back("--expect-warm");
    if (const auto msvc = env_or("MCPPLS_CTS_MSVC_ENV", ""); !msvc.empty())
        argv.insert(argv.end(), {"--msvc-env", msvc});
    if (const auto bundles = env_or("MCPPLS_CTS_BUNDLES", ""); !bundles.empty())
        argv.insert(argv.end(), {"--keep-bundles", bundles});
    const bool verbose = env_or("MCPPLS_CTS_VERBOSE", "") == "1";
    if (verbose)
        argv.push_back("--verbose");
    auto home = s.home;
    if (const auto evidence = env_or("MCPPLS_CTS_EVIDENCE", ""); s.evidence && !evidence.empty())
        // A diagnostic run keeps its own, beside the first run's.
        home = os::fs::from_utf8(evidence) / (file_part(id) + (verbose ? "-diagnostic" : ""));
    if (!home.empty())
        argv.insert(argv.end(), {"--workspace-dir", os::fs::utf8(home / "workspace"), "--cache-dir", os::fs::utf8(home / s.cache)});
    const auto name = measure_name(id, s);
    auto measure = mt::temp_dir() / (name + ".json");
    if (const auto measures = env_or("MCPPLS_CTS_MEASURES", ""); !measures.empty()) {
        std::filesystem::create_directories(os::fs::from_utf8(measures));
        measure = os::fs::from_utf8(measures) / (name + ".json");
    }
    argv.insert(argv.end(), s.arguments.begin(), s.arguments.end());
    argv.insert(argv.end(), {"--measure", os::fs::utf8(measure)});
    mt::e2e::command c{.argv = argv, .timeout = std::chrono::minutes{40}};
    if (s.linked) {
        const auto real = mt::temp_dir() / "real", link = mt::temp_dir() / "link";
        std::filesystem::create_directories(real);
        std::filesystem::create_directory_symlink(real, link);
        c.env.emplace_back("TMPDIR", os::fs::utf8(link));
    }
    const auto r = mt::e2e::run(std::move(c));
    const auto text = os::fs::read(measure);
    const auto measured = text ? json::parse(*text, nullptr, false) : json(nullptr);
    judge(r, measured, s.fixture);
    return measured.is_discarded() ? json(nullptr) : measured;
}

}  // namespace mcppls::harness
