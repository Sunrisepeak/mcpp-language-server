// `bench tokens`: the medians over rounds of the tokens-large-file measures, and the table beside a baseline.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.devtools.bench.tokens;

namespace bench = mcppls::devtools::bench;
using Json = nlohmann::json;

int main() {
    using namespace mcppls::testing;

    "the medians are per stage and metric, over the rounds that measured it"_test = [] {
        const Json rounds = Json::array({
            Json { { "cold", Json { { "firstTokens", 3.0 }, { "editBytes", 70 } } }, { "warm", Json { { "firstTokens", 0.1 } } } },
            Json { { "cold", Json { { "firstTokens", 1.0 }, { "editBytes", 72 } } }, { "warm", nullptr } },
            Json { { "cold", Json { { "firstTokens", 2.0 } } }, { "warm", Json { { "firstTokens", 0.3 } } } },
        });
        const Json medians = bench::token_medians(rounds);
        expect(medians["cold"]["firstTokens"] == 2.0);
        expect(medians["cold"]["editBytes"] == 71.0);
        expect(medians["warm"]["firstTokens"].get<double>() == 0.2);
        expect(medians["warm"]["editBytes"].is_null()) << "no round measured it";
    };

    "the table shows seconds, bytes and counts, with the baseline beside them"_test = [] {
        const Json medians { { "cold", Json { { "firstTokens", 2.5 }, { "editBytes", 71.0 }, { "polls", 0.0 } } }, { "warm", Json::object() } };
        const std::string alone { bench::token_table(medians, nullptr) };
        expect(alone.contains("| firstTokens | 2.50 s | - |"));
        expect(alone.contains("| editBytes | 71 | - |"));
        const Json baseline { { "cold", Json { { "editBytes", 1015373.0 } } }, { "warm", Json::object() } };
        const std::string compared { bench::token_table(medians, baseline) };
        expect(compared.starts_with("| metric | cold | cold (baseline) | warm | warm (baseline) |"));
        expect(compared.contains("| editBytes | 71 | 1015373 | - | - |"));
    };

    return report();
}
