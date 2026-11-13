// Issue 86 (design 2026-11-12): how an exit of clangd is named, whose it was, what the ledger holds against a file
// that ended it, and how many starts a session's sandbox has left.
import std;
import nlohmann.json;
import mcpplibs.mtest;
import mcppls.engine.clangd.exits;

namespace cld = mcppls::engine::clangd;
using Json = nlohmann::json;

using namespace mcpplibs::mtest;
using namespace std::chrono_literals;

UTest exits_tests{"exits", [] {

    "S1: an end by the system keeps the system's number, with the top bit Windows' platform clears put back"_test = [] {
        // 0xC000001D arrives as 0x4000001D, ended by the environment (openkal-windows' kal_process_wait).
        const auto illegal = cld::exit_condition(0x4000001D, true, cld::ExitHost::windows);
        expect(illegal.number == 0xC000001Du);
        expect(illegal.kind == cld::ExitKind::fault);
        expect(illegal.name == "illegal instruction");
        expect(illegal.text() == "an illegal instruction (0xC000001D)") << illegal.text();
        expect(illegal.number_text() == "0xC000001D");
        expect(cld::exit_condition(0x40000005, true, cld::ExitHost::windows).text() == "an access violation (0xC0000005)");
        expect(cld::exit_condition(0x400000FD, true, cld::ExitHost::windows).text() == "a stack overflow (0xC00000FD)");
        expect(cld::exit_condition(0x40000409, true, cld::ExitHost::windows).name == "stack buffer overrun");
        // The console's interrupt and a termination by the starter are ends from outside.
        expect(cld::exit_condition(0x4000013A, true, cld::ExitHost::windows).kind == cld::ExitKind::outside);
        const auto terminated = cld::exit_condition(0x7FFFFFFF, true, cld::ExitHost::windows);
        expect(terminated.number == 0xFFFFFFFFu && terminated.kind == cld::ExitKind::outside);
        // A status nobody named is printed as it is, and is still a fault.
        const auto unknown = cld::exit_condition(0x40000420, true, cld::ExitHost::windows);
        expect(unknown.kind == cld::ExitKind::fault && unknown.text() == "status 0xC0000420") << unknown.text();
        // A runner that returns its image's status: the number is the system's all the same.
        const auto returned = cld::exit_condition(static_cast<int>(0xC000001Du), false, cld::ExitHost::windows);
        expect(returned.kind == cld::ExitKind::fault && returned.name == "illegal instruction");
    };

    "S1: a signal is named, and SIGKILL, SIGTERM and SIGINT are ends from outside"_test = [] {
        const auto illegal = cld::exit_condition(4, true, cld::ExitHost::posix);
        expect(illegal.kind == cld::ExitKind::fault && illegal.text() == "SIGILL (signal 4)") << illegal.text();
        expect(cld::exit_condition(11, true, cld::ExitHost::posix).name == "SIGSEGV");
        expect(cld::exit_condition(6, true, cld::ExitHost::posix).name == "SIGABRT");
        for (const int outside : { 2, 9, 15 }) expect(cld::exit_condition(outside, true, cld::ExitHost::posix).kind == cld::ExitKind::outside) << outside;
        expect(cld::exit_condition(63, true, cld::ExitHost::posix).text() == "signal 63");
        const auto status = cld::exit_condition(3, false, cld::ExitHost::posix);
        expect(status.kind == cld::ExitKind::returned && status.text() == "status 3" && status.number_text() == "3");
    };

    "E2: the runner's report of a fault gives the condition and the frames as image offsets"_test = [] {
        const auto what = cld::runner_fault_line("openkal: fault: an instruction the program may not execute (SIGILL) at 0x3e8002928f9");
        expect(fatal(what.has_value()));
        expect(what->what == "an instruction the program may not execute (SIGILL)" && what->frame.empty()) << what->what;
        const auto frame = cld::runner_fault_line("openkal:   #1 0x1a3db4dafb30 /tmp/payload/clangd/openkal/bin/clangd.okb+0x5b3fb2f\r");
        expect(fatal(frame.has_value()));
        expect(frame->what.empty() && frame->frame == "clangd.okb+0x5b3fb2f") << frame->frame;
        const auto runner = cld::runner_fault_line("I[10:00:00.000] openkal:   #0 0x2a238d C:\\payload\\libexec\\okrun\\windows-x86_64\\okrun.exe+0xa138d");
        expect(runner.has_value() && runner->frame == "okrun.exe+0xa138d");
        expect(!cld::runner_fault_line("I[18:31:41.396] Built module std to /cache/std.pcm").has_value());
        expect(!cld::runner_fault_line("openkal:   #3 unknown").has_value());
        expect(!cld::runner_fault_line("openkal: fault: ").has_value());
    };

    "S2: the builds in flight are the ones that began and did not end"_test = [] {
        cld::BuildsInFlight building;
        building.began("/p/a.cppm", "/p/a.cppm");
        building.began("/p/b.cpp", "/p/b.cpp");
        building.began("/p/a.cppm", "/p/A.cppm");   // another version of the same file: one build, the later spelling
        expect(building.files() == std::vector<std::string> { "/p/A.cppm", "/p/b.cpp" });
        building.ended("/p/b.cpp");
        expect(building.files() == std::vector<std::string> { "/p/A.cppm" } && building.contains("/p/a.cppm") && !building.contains("/p/b.cpp"));
        building.clear();
        expect(building.size() == 0u);
    };

    "S2: the suspect is the file being built; a guess gets a strike, and an end from outside names nobody"_test = [] {
        const auto fault = cld::exit_condition(0x4000001D, true, cld::ExitHost::windows);
        // The report's exit of 01:21:32: one file building, three files visible. 0.22.0 recorded main.cpp.
        const auto one = cld::exit_suspects(fault, std::nullopt, { "src/ui/monitor_view.cppm" }, { "src/main.cpp", "src/ui/monitor_view.cppm" });
        expect(one.rule == cld::ExitSuspects::Rule::in_flight && one.certain);
        expect(one.files == std::vector<std::string> { "src/ui/monitor_view.cppm" });
        // Several in flight: each a strike, the next exit separates them.
        const auto several = cld::exit_suspects(fault, std::nullopt, { "a.cppm", "b.cpp" }, { "c.cpp" });
        expect(several.rule == cld::ExitSuspects::Rule::in_flight && !several.certain && several.files.size() == 2u);
        // Nothing in flight: what was asked about or edited, as before, a strike each.
        const auto guessed = cld::exit_suspects(fault, std::nullopt, {}, { "c.cpp" });
        expect(guessed.rule == cld::ExitSuspects::Rule::guessed && !guessed.certain && guessed.files == std::vector<std::string> { "c.cpp" });
        // clangd's own crash context comes first.
        const auto named = cld::exit_suspects(fault, std::string { "crasher.cpp" }, { "a.cppm" }, { "c.cpp" });
        expect(named.rule == cld::ExitSuspects::Rule::named && named.certain && named.files == std::vector<std::string> { "crasher.cpp" });
        // Killed: whatever it was building keeps clangd.
        const auto killed = cld::exit_suspects(cld::exit_condition(9, true, cld::ExitHost::posix), std::nullopt, { "a.cppm" }, { "c.cpp" });
        expect(killed.rule == cld::ExitSuspects::Rule::nobody && killed.files.empty());
        // A status the program returned says nothing of a fault: its one build in flight gets a strike, not the verdict.
        const auto status = cld::exit_suspects(cld::exit_condition(1, false, cld::ExitHost::posix), std::nullopt, { "a.cppm" }, {});
        expect(status.rule == cld::ExitSuspects::Rule::in_flight && !status.certain);
    };

    "S3: one retry per term, whatever the reason; unchanged is not eligible after the second exit"_test = [] {
        cld::CrashLedger ledger;
        using Verdict = cld::CrashLedger::Verdict;
        const auto t0 = cld::LedgerClock::time_point { std::chrono::hours { 500000 } };
        expect(ledger.verdict("k", "engine-1", "text-1", "closure-1", t0) == Verdict::clean);
        const auto& first = ledger.exited("k", "/p/monitor_view.cppm", "engine-1", "an illegal instruction (0xC000001D)", "text-1", "closure-1", t0);
        expect(first.exits == 1 && first.term == 2min && first.retryAt == t0 + 2min);
        // Within the term nothing sends it back: not an edit, not a rebuilt closure.
        expect(ledger.verdict("k", "engine-1", "text-2", "closure-2", t0 + 1min) == Verdict::held);
        // The term over, a file that ended the engine once goes back once, changed or not.
        expect(ledger.verdict("k", "engine-1", "text-1", "closure-1", t0 + 2min) == Verdict::retry);
        // It ends the engine again: the term doubles.
        const auto& second = ledger.exited("k", "/p/monitor_view.cppm", "engine-1", "an illegal instruction (0xC000001D)", "text-1", "closure-1", t0 + 3min);
        expect(second.exits == 2 && second.term == 4min && second.firstAt == t0 && second.lastAt == t0 + 3min);
        expect(ledger.verdict("k", "engine-1", "text-2", "closure-1", t0 + 5min) == Verdict::held) << "an edit inside the term";
        // The term over and nothing changed: nothing could change the outcome, so it waits.
        expect(ledger.verdict("k", "engine-1", "text-1", "closure-1", t0 + 8min) == Verdict::held);
        expect(ledger.verdict("k", "engine-1", "text-1", "closure-1", t0 + 24h) == Verdict::held);
        // A change -- the text, or what its modules were built from -- and it goes back.
        expect(ledger.verdict("k", "engine-1", "text-2", "closure-1", t0 + 8min) == Verdict::retry);
        expect(ledger.verdict("k", "engine-1", "text-1", "closure-2", t0 + 8min) == Verdict::retry);
        // The terms: 2, 4, 8, 16 minutes, an hour, a day, and a day from then on.
        std::vector<std::chrono::minutes> terms { 2min, 4min };
        for (int i { 0 }; i < 6; ++i) terms.push_back(ledger.exited("k", "/p/monitor_view.cppm", "engine-1", "x", "t", "c", t0 + 10min).term);
        expect(terms == std::vector<std::chrono::minutes> { 2min, 4min, 8min, 16min, 60min, 1440min, 1440min, 1440min });
    };

    "S3: another engine starts clean; the person's try again and a build that did not end clangd clear the hold"_test = [] {
        cld::CrashLedger ledger;
        using Verdict = cld::CrashLedger::Verdict;
        const auto t0 = cld::LedgerClock::time_point { std::chrono::hours { 500000 } };
        ledger.exited("k", "/p/a.cppm", "engine-1", "SIGILL (signal 4)", "text", "closure", t0);
        ledger.exited("k", "/p/a.cppm", "engine-1", "SIGILL (signal 4)", "text", "closure", t0);
        expect(ledger.verdict("k", "engine-2", "text", "closure", t0) == Verdict::clean) << "the image's sha256 changed";
        expect(ledger.verdict("k", "engine-1", "text", "closure", t0) == Verdict::held);
        expect(ledger.try_again("k"));
        expect(ledger.verdict("k", "engine-1", "text", "closure", t0) == Verdict::retry) << "inside the term and unchanged: the person decided";
        // It ended the engine again: the request is spent.
        expect(ledger.exited("k", "/p/a.cppm", "engine-1", "SIGILL (signal 4)", "text", "closure", t0 + 1min).exits == 3);
        expect(ledger.verdict("k", "engine-1", "text", "closure", t0 + 2min) == Verdict::held);
        expect(ledger.survived("k"));
        expect(ledger.verdict("k", "engine-1", "text", "closure", t0 + 2min) == Verdict::clean && ledger.empty());
        expect(!ledger.try_again("k") && !ledger.survived("k"));
        // A record of another engine is replaced, not continued.
        ledger.exited("k", "/p/a.cppm", "engine-1", "x", "t", "c", t0);
        expect(ledger.exited("k", "/p/a.cppm", "engine-2", "x", "t", "c", t0).exits == 1);
    };

    "S3: the ledger is a file: what was written is what a new session reads, and old or foreign records are pruned"_test = [] {
        cld::CrashLedger ledger;
        const auto t0 = cld::LedgerClock::time_point { std::chrono::hours { 500000 } };
        ledger.exited("/p/a.cppm", "/p/A.cppm", "engine-1", "an illegal instruction (0xC000001D)", "text", "closure", t0);
        ledger.exited("/p/a.cppm", "/p/A.cppm", "engine-1", "an illegal instruction (0xC000001D)", "text", "closure", t0 + 1min);
        ledger.exited("/p/b.cpp", "/p/b.cpp", "engine-0", "SIGSEGV (signal 11)", "text", "closure", t0);
        const Json written = Json::parse(ledger.to_json().dump());
        expect(written["version"] == 1 && written["files"].size() == 2u);
        expect(written["files"]["/p/a.cppm"]["exits"] == 2 && written["files"]["/p/a.cppm"]["term"] == 4);
        cld::CrashLedger read;
        read.from_json(written);
        expect(fatal(read.size() == 2u));
        const cld::CrashRecord* record { read.find("/p/a.cppm") };
        expect(fatal(record != nullptr));
        expect(record->file == "/p/A.cppm" && record->exits == 2 && record->term == 4min && record->retryAt == t0 + 5min
               && record->condition == "an illegal instruction (0xC000001D)" && record->content == "text" && record->closure == "closure");
        using Verdict = cld::CrashLedger::Verdict;
        expect(read.verdict("/p/a.cppm", "engine-1", "text", "closure", t0 + 2min) == Verdict::held) << "a new session does not hand it to clangd";
        expect(read.prune("engine-1", t0 + 2min)) << "another engine's record goes";
        expect(read.size() == 1u && read.find("/p/b.cpp") == nullptr);
        expect(!read.prune("engine-1", t0 + 24h * 29));
        expect(read.prune("engine-1", t0 + 24h * 31) && read.empty()) << "thirty days";
        // Anything else is an empty ledger, not an error.
        for (const auto& bad : { Json(nullptr), Json::array(), Json { { "version", 2 }, { "files", Json::object() } }, Json { { "version", 1 }, { "files", 3 } },
                                 Json { { "version", 1 }, { "files", Json { { "k", Json { { "file", "" }, { "engine", "e" }, { "exits", 1 } } } } } } }) {
            cld::CrashLedger empty;
            empty.from_json(bad);
            expect(empty.empty()) << bad.dump();
        }
    };

    "S4: the reserve is three starts; the counts are the starts made and the slots they spent"_test = [] {
        // The report: four read-only grants a start, a table of 64, the 17th start refused.
        cld::StartBudget budget { cld::StartBudget::CAPACITY };
        expect(budget.left(4) == std::optional<std::size_t> { 16 });
        for (int start { 1 }; start <= 12; ++start) budget.started(4);
        expect(budget.starts() == 12u && budget.spent() == 48u && budget.left(4) == std::optional<std::size_t> { 4 });
        expect(budget.automatic(4)) << "four are left: one more than the reserve";
        budget.started(4);
        expect(budget.left(4) == std::optional<std::size_t> { 3 } && !budget.automatic(4)) << "the reserve is the person's";
        expect(budget.by_person(4));
        for (int start { 0 }; start < 3; ++start) budget.started(4);
        expect(budget.starts() == 16u && budget.left(4) == std::optional<std::size_t> { 0 } && !budget.by_person(4)) << "the 17th is the one refused";
        // CI's runners: three a start, 21 fit and the 22nd is refused.
        cld::StartBudget three { cld::StartBudget::CAPACITY };
        for (int start { 0 }; start < 21; ++start) three.started(3);
        expect(three.left(3) == std::optional<std::size_t> { 0 });
        // A system that counts nothing never runs out.
        cld::StartBudget uncounted;
        for (int start { 0 }; start < 100; ++start) uncounted.started(9);
        expect(!uncounted.counted() && !uncounted.left(9).has_value() && uncounted.automatic(9) && uncounted.by_person(9) && uncounted.starts() == 100u);
    };
}};
