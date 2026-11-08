// The user-experience scenarios (0.0.7 plan 6.4; tests/cts/README.md, "Scenario checks"): a real
// project -- mcpp's repository (ux-mcpp), xlings' (ux-xlings), a generated one of heavy headers
// (ux-heavy-headers) -- opened as a person opens it: cold, warm on the cold one's cache, edited, and
// with faults on a cache of its own. Each stage is a case; the scenario's checks hold its budgets,
// and each check's time and the numbers it measured (a completion's 95th percentile, a refresh, the
// engine's share) are metrics of its stage. The workspace is fetched and prepared by the first
// stage and used as it is by the rest; a stage puts back the files it changed.
import std;
import nlohmann.json;
import mcpplibs.mtest;
import mcppls.harness;

using namespace mcpplibs::mtest;
using mcppls::harness::on;
using json = nlohmann::json;

namespace {

struct stage {
    std::string_view name;
    std::string_view cache;
    bool warm;
};

constexpr std::array<stage, 4> sessions{{{"cold", "cache-a", false},
                                         {"warm", "cache-a", true},
                                         {"edits", "cache-a", true},
                                         {"faults", "cache-b", false}}};

// A measure's numbers as metrics under `prefix`: a number, or the numbers of the objects it holds,
// two levels down (`completion.p95`).
void numbers(const std::string& prefix, const json& value, int depth) {
    if (value.is_number())
        metric(prefix, value.get<double>(), "1");
    else if (value.is_object() && depth < 2)
        for (const auto& entry : value.items())
            numbers(prefix + "." + entry.key(), entry.value(), depth + 1);
}

// What a stage measured, as metrics: each check's seconds, and the numbers of its measure.
void report(const json& measured) {
    for (const auto& check : measured.is_object() ? measured.value("checks", json::array()) : json::array()) {
        const auto id = check.value("id", std::string{});
        if (check.contains("seconds") && check["seconds"].is_number())
            metric(id + ".seconds", check["seconds"].get<double>(), "s");
        numbers(id, check.value("measure", json::object()), 0);
    }
}

// The directory the stages share -- the workspace and its caches: under MCPPLS_CTS_EVIDENCE, kept
// for the record (the server's logs and incidents), else a temporary one removed with the case.
struct scratch {
    std::filesystem::path dir;
    bool kept;
    explicit scratch(std::string_view fixture)
        : dir(mcppls::harness::env_or("MCPPLS_CTS_EVIDENCE", "").empty()
                  ? std::filesystem::temp_directory_path() /
                        std::format("mcppls-bench-{}-{}", fixture, std::chrono::steady_clock::now().time_since_epoch().count())
                  : std::filesystem::path(mcppls::harness::env_or("MCPPLS_CTS_EVIDENCE", "")) / fixture),
          kept(!mcppls::harness::env_or("MCPPLS_CTS_EVIDENCE", "").empty()) {
        std::filesystem::create_directories(dir);
    }
    scratch(const scratch&) = delete;
    ~scratch() {
        std::error_code ignored;
        if (!kept)
            std::filesystem::remove_all(dir, ignored);
    }
};

void scenario(std::string_view fixture, std::span<const stage> stages) {
    shared<scratch> work{"workspace", [fixture] { return fixture; }};
    for (const auto& s : stages)
        test(std::string(s.name)) = [&] {
            report(mcppls::harness::run({.fixture = std::string(fixture), .stage = std::string(s.name), .warm = s.warm,
                                         .home = work->dir, .cache = std::string(s.cache), .timeout = 900}));
        };
}

Bench ux_mcpp{"ux-mcpp", on{"linux-x64", "darwin-arm64", "win32-x64"}, tag{"ux"}, [] { scenario("ux-mcpp", sessions); }};
Bench ux_xlings{"ux-xlings", on{"linux-x64"}, tag{"ux"}, [] { scenario("ux-xlings", sessions); }};

// 0.0.9 plan M-2: one session, of a generated project: completion through the server against clangd alone.
Bench ux_heavy_headers{"ux-heavy-headers", on{"linux-x64"}, tag{"ux"}, [] {
    constexpr std::array<stage, 1> edits{{{"edits", "cache-a", false}}};
    scenario("ux-heavy-headers", edits);
}};

}  // namespace
