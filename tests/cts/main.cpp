// The conformance cases as one program, `mcppls-cts` (README.md): the cases are the objects of this
// directory's topic files (mcpp.cpp, targets.cpp, cmake.cpp, xmake.cpp, inferred.cpp, engine.cpp,
// editing.cpp), each a fixture of fixtures/ run by mcppls-conformance (cts.cppm, mcppls.harness,
// whose head names the environment the cases run against). The program speaks the mtest
// protocol, so mtest lists, selects, schedules and records the cases; alone it runs them one
// after another.
import std;
import nlohmann.json;
import mcpplibs.os;
import mcpplibs.mtest.manifest;
import mcppls.cts;

using namespace mcpplibs::mtest;
using namespace mcppls::cts;
namespace os = mcpplibs::os;
namespace harness = mcppls::harness;
using json = nlohmann::json;

namespace {

// Whether a fixture's checks bound how long an answer takes, which says something about the server
// only on a machine nothing else uses.
bool times_answers(const json& scenario) {
    static const std::set<std::string> kinds{"stress", "typing", "tokens-latency"};
    static const std::array<std::string_view, 6> keys{"answer-within", "within-ms", "within-seconds", "within",
                                                      "budget", "initialize-within"};
    if (scenario.contains("initialize-within"))
        return true;
    return std::ranges::any_of(scenario.value("checks", json::array()), [&](const json& c) {
        return c.is_object() && (kinds.contains(c.value("kind", "")) ||
                                 std::ranges::any_of(keys, [&](std::string_view k) { return c.contains(k); }));
    });
}

// The sources of this directory, as `mtest list --static` reads them.
std::vector<std::string> sources(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".cpp" || entry.path().extension() == ".cppm")
            files.push_back(entry.path());
    std::ranges::sort(files);
    std::vector<std::string> texts;
    for (const auto& file : files)
        if (auto text = os::fs::read(file))
            texts.push_back(std::move(*text));
    return texts;
}

// The declarations agree with the fixtures: every case names a fixture there is, and a fixture
// whose checks bound an answer's time holds the machine. Read from this directory's text (the
// static manifest), as `mtest list --static` reads it.
CTS declarations{"the cases and the fixtures agree", on{"linux-x64"}, [] {
    const auto here = os::fs::from_utf8("tests/cts");
    if (!std::filesystem::exists(here / "cts.cppm"))
        skip("tests/cts is not in the working directory");
    const auto texts = sources(here);
    const auto types = mcpplibs::mtest::manifest::types_of(texts);
    const auto fixtures = os::fs::from_utf8(harness::env_or("MCPPLS_CTS_FIXTURES", "tests/cts/fixtures"));
    std::size_t seen = 0;
    for (const auto& text : texts) {
        for (const auto& c : mcpplibs::mtest::manifest::scan(text, types)) {
            if (c.kind != "cts" || c.id.find('/') != std::string::npos || c.id == current_case())
                continue;
            ++seen;
            const auto scenario = os::fs::read(fixtures / c.id.substr(0, c.id.find('@')) / "scenario.json");
            expect(scenario.has_value()) << c.id << " names no fixture of " << os::fs::utf8(fixtures);
            const bool holds = std::ranges::find(c.resources, "machine") != c.resources.end();
            expect(!scenario || holds || !times_answers(json::parse(*scenario, nullptr, false)))
                << c.id << " bounds how long an answer takes and holds no machine (resource{\"machine\"})";
        }
    }
    expect(gt(seen, std::size_t(100))) << "the manifest read " << seen << " cases";
}};

}  // namespace

int main(int argc, char** argv) {
    return run(argc, argv);
}
