// Real-project stress testing (real-project plan RP0): the devtools `stress` command's
// matrix, row-extraction and rendering logic, without running a real server (that is proven by
// running the command itself against a payload, which a unit test cannot assume it has).
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.devtools.stress;

namespace stress = mcppls::devtools::stress;
using Json = nlohmann::json;

int main() {
    using namespace mcppls::testing;

    "the matrix is fixtures times clients, in order, with duplicates dropped"_test = [&] {
        auto specs = stress::matrix({ "inferred", "module-faults", "inferred" }, {}, { "vscode", "plain", "vscode" });
        expect(fatal(specs.has_value())) << (specs ? "" : specs.error().message);
        expect(specs->size() == 4u) << specs->size();
        expect((*specs)[0].fixture == "inferred" && (*specs)[0].client == "vscode");
        expect((*specs)[1].fixture == "inferred" && (*specs)[1].client == "plain");
        expect((*specs)[2].fixture == "module-faults" && (*specs)[2].client == "vscode");
        expect((*specs)[3].fixture == "module-faults" && (*specs)[3].client == "plain");
    };

    "no --client at all runs once with the default profile, named \"none\""_test = [&] {
        auto specs = stress::matrix({ "inferred" }, {}, {});
        expect(fatal(specs.has_value()));
        expect(specs->size() == 1u);
        expect((*specs)[0].client == "none");
    };

    "--project mcpp is the self-mcpp fixture; an unknown project is an error"_test = [&] {
        auto specs = stress::matrix({}, { "mcpp" }, {});
        expect(fatal(specs.has_value()));
        expect(specs->size() == 1u);
        expect((*specs)[0].fixture == "self-mcpp");

        auto bad = stress::matrix({}, { "not-a-real-project" }, {});
        expect(!bad.has_value());
    };

    "neither --fixture nor --project is an error, not an empty matrix"_test = [&] {
        auto specs = stress::matrix({}, {}, { "plain" });
        expect(!specs.has_value());
    };

    "extract_row sums per-method counts and reads the timeline/process numbers"_test = [&] {
        const Json detail {
            { "methods", { { "textDocument/hover", { { "answered", 3 }, { "empty", 1 }, { "timeout", 0 }, { "error", 0 } } },
                          { "textDocument/definition", { { "answered", 2 }, { "empty", 0 }, { "timeout", 1 }, { "error", 1 } } } } },
            { "p90", 0.42 },
            { "maxStallSeconds", 1.5 },
            { "cpuSeconds", 3.2 },
            { "rssMB", 128.5 },
        };
        const Json measured { { "checks", Json::array({ Json { { "id", "C1" }, { "kind", "hover-contains" }, { "ok", true } },
                                                       Json { { "id", "STRESS1" }, { "kind", "stress" }, { "ok", false }, { "detail", detail.dump() } } }) } };
        auto row = stress::extract_row("inferred", "plain", measured);
        expect(fatal(row.has_value())) << (row ? "" : row.error().message);
        expect(!row->ok);
        expect(row->answered == 5) << row->answered;
        expect(row->empty == 1) << row->empty;
        expect(row->timeout == 1) << row->timeout;
        expect(row->error == 1) << row->error;
        // Through a JSON dump and parse: a decimal like 0.42 need not come back as the very same double
        // on every platform, so values are compared to a tolerance, not for identity.
        const auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        expect(near(row->p90, 0.42)) << row->p90;
        expect(near(row->maxStallSeconds, 1.5));
        expect(fatal(row->cpuSeconds.has_value()));
        expect(near(*row->cpuSeconds, 3.2)) << *row->cpuSeconds;
        expect(fatal(row->rssMB.has_value()));
        expect(near(*row->rssMB, 128.5));
    };

    "extract_row fails on a measure document with no stress check, or none at all"_test = [&] {
        auto noStress = stress::extract_row("inferred", "plain", Json { { "checks", Json::array({ Json { { "kind", "hover-contains" } } }) } });
        expect(!noStress.has_value());
        auto noChecks = stress::extract_row("inferred", "plain", Json::object());
        expect(!noChecks.has_value());
    };

    "extract_row notes rather than fails when the stress check's own detail is not the summary"_test = [&] {
        const Json measured { { "checks", Json::array({ Json { { "kind", "stress" }, { "ok", false }, { "detail", "no files matched" } } }) } };
        auto row = stress::extract_row("inferred", "plain", measured);
        expect(fatal(row.has_value()));
        expect(!row->ok);
        expect(!row->note.empty());
    };

    "a row round-trips through JSON, including a null cpuSeconds/rssMB and a note"_test = [&] {
        stress::StressRow row { .fixture = "inferred", .client = "zed", .ok = false, .answered = 4, .empty = 1, .timeout = 2,
                                .error = 0, .p90 = 0.1, .maxStallSeconds = 2.0, .note = "over budget" };
        const Json encoded = stress::to_json(row);   // `Json x { y }` would wrap y in a one-element array; `=` copies it
        expect(encoded["cpuSeconds"].is_null());
        expect(encoded["rssMB"].is_null());
        auto decoded = stress::rows_from_json(Json::array({ encoded }));
        expect(fatal(decoded.has_value()));
        expect(decoded->size() == 1u);
        const auto& back = decoded->front();
        expect(back.fixture == row.fixture && back.client == row.client);
        expect(!back.cpuSeconds.has_value() && !back.rssMB.has_value());
        expect(back.note == row.note);
        expect(back.answered == 4 && back.timeout == 2);
    };

    "the table names every fixture/client and a row's note instead of numbers it does not have"_test = [&] {
        std::vector<stress::StressRow> rows {
            { .fixture = "inferred", .client = "vscode", .ok = true, .answered = 10, .p90 = 0.05, .cpuSeconds = 1.0, .rssMB = 100.0 },
            { .fixture = "real-xlings", .client = "plain", .ok = false, .note = "no fixture at .../real-xlings" },
        };
        const std::string table { stress::render_table(rows) };
        expect(table.contains("inferred"));
        expect(table.contains("vscode"));
        expect(table.contains("real-xlings"));
        expect(table.contains("no fixture at"));
    };

    "the comparison shows a delta for a matched row and flags new/missing ones"_test = [&] {
        std::vector<stress::StressRow> baseline { { .fixture = "inferred", .client = "plain", .p90 = 0.10, .maxStallSeconds = 1.0, .timeout = 0 },
                                                  { .fixture = "module-faults", .client = "plain", .p90 = 0.20 } };
        std::vector<stress::StressRow> current { { .fixture = "inferred", .client = "plain", .p90 = 0.15, .maxStallSeconds = 0.5, .timeout = 1 },
                                                 { .fixture = "generated-module", .client = "plain", .p90 = 0.05 } };
        const std::string diff { stress::render_comparison(baseline, current) };
        expect(diff.contains("+0.05")) << diff;     // inferred's p90 grew by 0.05s
        expect(diff.contains("generated-module") && diff.contains("(new)")) << diff;
        expect(diff.contains("module-faults") && diff.contains("(missing from this run)")) << diff;
    };

    return report();
}
