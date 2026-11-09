// The semantic tokens of a large file (mcpp design 2026-10-28 §11.1): the tokens-large-file
// fixture's cold stage, then its warm stage on the cache the cold one left. The fixture's checks
// are the design's criteria -- on a warm open the compiler's tokens come no later than the first
// completion, an edit's answer is a delta under 5% of the file's, and updates come by refresh --
// and each stage's numbers are metrics: how soon the first completion and the compiler's tokens
// come, the bytes of the whole and of an edit's answer, the refreshes and the polls.
import std;
import nlohmann.json;
import mcpplibs.mtest;
import mcppls.harness;

using namespace mcpplibs::mtest;
using mcppls::harness::on;
using json = nlohmann::json;

namespace {

constexpr std::array<std::string_view, 10> numbers{
    "firstCompletion", "firstTokens", "tokensAfterCompletion", "fullBytes", "editFirstAnswer",
    "editUpdated",     "editBytes",   "refreshes",             "polls",     "missedRefreshes"};

std::string_view unit_of(std::string_view number) {
    if (number.ends_with("Bytes"))
        return "B";
    if (number == "refreshes" || number == "polls" || number == "missedRefreshes")
        return "1";
    return "s";
}

// The tokens-latency check's measure in what the runner measured, or null.
json tokens_measure(const json& measured) {
    for (const auto& check : measured.is_object() ? measured.value("checks", json::array()) : json::array())
        if (check.is_object() && check.contains("measure") && check["measure"].is_object() &&
            check["measure"].contains("firstTokens"))
            return check["measure"];
    return nullptr;
}

Bench tokens{"tokens", on{"linux-x64"}, [] {
    const auto home = temp_dir();
    for (const std::string stage : {"cold", "warm"}) {
        const auto measure = tokens_measure(
            mcppls::harness::run({.fixture = "tokens-large-file", .stage = stage, .warm = stage == "warm", .home = home, .timeout = 300}));
        expect(fatal(measure.is_object())) << "the " << stage << " stage measured no tokens";
        for (const auto number : numbers)
            if (const auto it = measure.find(std::string(number)); it != measure.end() && it->is_number())
                metric(std::format("{}.{}", stage, number), it->get<double>(), unit_of(number));
    }
}};

}  // namespace
