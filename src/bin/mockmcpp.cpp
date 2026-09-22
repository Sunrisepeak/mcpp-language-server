// mcppls-mock-mcpp: stands in for mcpp in conformance fixtures. It answers the
// machine-output contract of mcpp-community/mcpp#636 from data a fixture recorded in
// mcpp-mock.json, for what a real mcpp cannot be made to do on demand: fail, change
// its answer, or take std from a package at a path of the fixture's choosing. mcpp
// 2026.9.15.1 ships the command; the recorded data is its output.
//
//   mcppls-mock-mcpp --protocol-version
//   mcppls-mock-mcpp emit build-database --format json
//
// In mcpp-mock.json every string may use ${root} (the directory the command runs
// in) and ${env:NAME} or ${env:NAME|fallback}. {"database": <S1>, "watch": [...]}
// is answered as an envelope; {"diagnostics": [...]} as a failure with exit 1; {"unavailable": "..."}
// as xlings answers for an mcpp a project pins but that is not installed. {"hang": {...}} is the
// failure the runner exists for: a producer that does not return, and that leaves something behind
// holding the caller's pipe, which is what one hung index refresh did to an editor for twelve
// minutes (build description design 1.1). {"oldProtocol": true} is mcpp 2026.8.8.4, before
// mcpp-community/mcpp#636: `--protocol-version` advertises no `mcpp.build-database` kind, `emit
// build-database` does not exist, and `build --configure-only` is rejected ("error: unknown
// option: --configure-only", exit 2) — real-project plan RP0,
// exercising the server's L2 fallback onto the project's own compile_commands.json.
//
// The config read is normally the project's own mcpp-mock.json (the current directory, which is
// the project root for every command a real mcpp would run there). A copy of this program placed
// where producer negotiation looks for another installed mcpp (real-project plan RP2.1,
// `xim-x-mcpp/<version>/bin/mcpp`) still runs with the *project's* directory as its cwd, so it
// reads a config beside its own executable instead when a fixture put one there -- a negotiated
// candidate's own version and database, distinct from the project's own mcpp-mock.json.
import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;

namespace base = mcppls::base;
namespace fs = mcppls::platform::fs;
using Json = nlohmann::ordered_json;

namespace {

constexpr std::string_view VERSION { "0.0.0-mock" };

std::string expand(std::string_view text, const std::string& root) {
    std::string out;
    for (std::size_t i { 0 }; i < text.size();) {
        if (text.substr(i).starts_with("${")) {
            const std::size_t close { text.find('}', i) };
            if (close != std::string_view::npos) {
                const std::string_view name { text.substr(i + 2, close - i - 2) };
                if (name == "root") {
                    out += root;
                } else if (name.starts_with("env:")) {
                    std::string_view variable { name.substr(4) };
                    std::string fallback;
                    if (const std::size_t bar { variable.find('|') }; bar != std::string_view::npos) {
                        fallback = std::string { variable.substr(bar + 1) };
                        variable = variable.substr(0, bar);
                    }
                    out += base::normalize_path(mcppls::platform::env::get(variable).value_or(fallback));
                } else {
                    out += text.substr(i, close - i + 1);
                }
                i = close + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

void expand_all(Json& value, const std::string& root) {
    if (value.is_string()) {
        value = expand(value.get<std::string>(), root);
    } else if (value.is_array() || value.is_object()) {
        for (auto& child : value) expand_all(child, root);
    }
}

// real-project plan RP2.1: a candidate mcpp that producer negotiation finds beside another
// project's own (`other_mcpp_executables`, `xim-x-mcpp/<version>/bin/mcpp`) still runs with that
// project's directory as its cwd, so `mcpp-mock.json` there is the *other* mcpp's recording, not
// this candidate's own. A config file beside this program's own executable, when one exists, is
// what a fixture puts there to give a negotiated candidate its own version and database; every
// fixture that runs the mock as the project's one and only mcpp keeps reading the project's.
std::string mock_config_path() {
    const auto arguments = mcppls::platform::env::arguments();
    if (!arguments.empty()) {
        const std::string& self { arguments.front() };
        const std::string absolute { base::is_absolute_path(self) ? base::normalize_path(self)
                                                                   : base::normalize_path(base::join_path(fs::current_directory(), self)) };
        const std::string beside { base::join_path(base::parent_path(absolute), "mcpp-mock.json") };
        if (fs::is_regular_file(beside)) return beside;
    }
    return base::join_path(fs::current_directory(), "mcpp-mock.json");
}

Json envelope(std::string_view kind, Json data, Json diagnostics, Json effects) {
    Json document = Json::object();
    document["schemaVersion"] = 1;
    document["kind"] = kind;
    document["kindVersion"] = 1;
    document["effects"] = std::move(effects);
    document["mcpp"] = Json { { "version", VERSION }, { "protocol", Json { { "min", 1 }, { "max", 1 } } } };
    if (!data.is_null()) document["data"] = std::move(data);
    document["diagnostics"] = std::move(diagnostics);
    return document;
}

// FNV-1a over the files a real producer would read, so the answer changes when they do.
std::string fingerprint(const std::string& root) {
    std::uint64_t hash { 1469598103934665603ull };
    auto mix = [&](std::string_view bytes) {
        for (const unsigned char c : bytes) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
    };
    static constexpr std::array<std::string_view, 5> SOURCES { ".cppm", ".cpp", ".ixx", ".cc", ".cxx" };
    for (const auto& file : fs::list_files(root, SOURCES, std::array<std::string_view, 1> { "target" })) {
        mix(file);
        mix(fs::read_file(file).value_or(""));
    }
    mix(fs::read_file(base::join_path(root, "mcpp.toml")).value_or(""));
    return std::format("fnv1a:{:016x}", hash);
}

// `{"oldProtocol": true}` in mcpp-mock.json: this mcpp predates mcpp-community/mcpp#636 (mcpp
// 2026.8.8.4, before mcpp.build-database and `emit build-database` existed), so the server's L2
// fallback is exercised — `mcpp build --configure-only` (below) instead, and where even that fails
// or the project has none of its own, the project's already-there compile_commands.json.
int protocol_version(bool oldProtocol) {
    Json document = Json::object();
    document["schemaVersion"] = 1;
    document["kind"] = "mcpp.protocol";
    document["envelope"] = Json { { "min", 1 }, { "max", 1 } };
    document["kinds"] = oldProtocol ? Json::object() : Json { { "mcpp.build-database", 1 } };
    // As mcpp 2026.9.15.1 declares it; 2026.8.8.4 has no `emit build-database` at all.
    document["commands"] = oldProtocol ? Json::object()
                                       : Json { { "emit build-database", Json { { "effects", Json::array({ "init-mcpp-home", "read-project", "network",
                                                                                                            "write-global-cache", "exec-build-script" }) } } } };
    document["mcpp"] = Json { { "version", oldProtocol ? std::string { "2026.8.8.4" } : std::string { VERSION } } };
    std::println("{}", document.dump(2));
    return 0;
}

int emit_build_database(std::span<const std::string> arguments) {
    std::string format;
    for (std::size_t i { 0 }; i < arguments.size(); ++i) {
        if (arguments[i] == "--format" && i + 1 < arguments.size()) format = arguments[++i];
        else if (arguments[i].starts_with("--format=")) format = arguments[i].substr(9);
    }
    if (format != "json") {
        std::println(std::cerr, "error: unsupported --format '{}'; expected: json", format);
        return 2;
    }
    const std::string root { base::normalize_path(fs::current_directory()) };
    auto text = fs::read_file(mock_config_path());
    if (!text) {
        const Json diagnostics = Json::array({ Json { { "code", "MCPP_MOCK_NO_DATA" }, { "severity", "error" }, { "source", "mcpp" },
                                                      { "message", "this fixture recorded no mcpp-mock.json" } } });
        std::println("{}", envelope("mcpp.build-database", nullptr, diagnostics, Json::array({ "read-project" })).dump(2));
        return 1;
    }
    Json recorded = Json::parse(*text, nullptr, false);
    if (recorded.is_discarded() || !recorded.is_object()) {
        std::println(std::cerr, "error: mcpp-mock.json is not a JSON object");
        return 70;
    }
    // Build description design 4.4: a run the server starts by itself must reach no network, which
    // for mcpp means MCPP_OFFLINE in its environment. A fixture that says `requireOffline` fails
    // unless it is there, so "offline by default" is checked rather than believed.
    if (recorded.value("requireOffline", false)) {
        const auto offline = mcppls::platform::env::get("MCPP_OFFLINE");
        if (!offline || offline->empty() || *offline == "0") {
            const Json diagnostics = Json::array({ Json { { "code", "MCPP_MOCK_NOT_OFFLINE" }, { "severity", "error" }, { "source", "mcpp" },
                                                          { "message", "this fixture requires MCPP_OFFLINE in the producer's environment and it was not set" } } });
            std::println("{}", envelope("mcpp.build-database", nullptr, diagnostics, Json::array({ "read-project" })).dump(2));
            return 1;
        }
    }
    expand_all(recorded, root);
    if (!recorded.contains("database")) {
        std::println("{}", envelope("mcpp.build-database", nullptr, recorded.value("diagnostics", Json::array()), Json::array({ "read-project" })).dump(2));
        return 1;
    }
    Json data = Json::object();
    data["database"] = recorded["database"];
    data["watch"] = recorded.value("watch", Json::array({ "mcpp.toml", "mcpp.lock", "src/**/*.cppm", "src/**/*.cpp" }));
    data["inputs-fingerprint"] = fingerprint(root);
    std::println("{}", envelope("mcpp.build-database", std::move(data), Json::array(), Json::array({ "read-project" })).dump(2));
    return 0;
}

} // namespace

// {"hang": {"seconds": 30, "holdPipe": true, "exitAtOnce": false}}
//
// `holdPipe` starts a descendant that inherits this program's standard error --- the caller's pipe
// --- as a unit of its own, so no group kill can reach it and only a bounded read frees the caller.
// `exitAtOnce` leaves at once and lets that descendant be the only thing holding the pipe.
int hang(const Json& how, std::span<const std::string> self) {
    const int seconds { how.value("seconds", 30) };
    if (how.value("holdPipe", true) && !self.empty()) {
        auto child = mcppls::platform::Process::spawn({ .program = std::string { self.front() },
                                                        .arguments = { "--mock-hold", std::to_string(seconds) },
                                                        .pipeInput = false, .pipeOutput = false, .pipeError = false,
                                                        .detached = true });
        if (child) child->detach();
    }
    if (how.value("exitAtOnce", false)) return 0;
    std::this_thread::sleep_for(std::chrono::seconds { seconds });
    return 0;
}

int main(int argc, char* argv[]) {
    const std::vector<std::string> arguments { argv + 1, argv + argc };
    // The descendant of a hang: it only has to stay alive, holding whatever it was given.
    if (arguments.size() == 2 && arguments[0] == "--mock-hold") {
        std::this_thread::sleep_for(std::chrono::seconds { std::max(1, std::stoi(arguments[1])) });
        return 0;
    }
    bool oldProtocol { false };
    // {"unavailable": "<text>"}: a project whose .xlings.json asks for an mcpp that is not installed.
    // xlings then answers every command in mcpp's place, with <text> on standard error, and runs nothing.
    if (auto text = fs::read_file(mock_config_path())) {
        const Json recorded = Json::parse(*text, nullptr, false);
        if (recorded.is_object() && recorded.contains("unavailable") && recorded["unavailable"].is_string()) {
            std::print(std::cerr, "{}", recorded["unavailable"].get<std::string>());
            return 1;
        }
        // Only the command the server waits on hangs; `--protocol-version` must still answer, or
        // the server would never get as far as asking for a database.
        if (recorded.is_object() && recorded.contains("hang") && recorded["hang"].is_object()
            && arguments.size() >= 2 && arguments[0] == "emit" && arguments[1] == "build-database") {
            const std::string self { mcppls::platform::env::arguments().empty() ? std::string {}
                                                                                : mcppls::platform::env::arguments().front() };
            const std::string absolute { base::is_absolute_path(self) ? base::normalize_path(self)
                                                                      : base::normalize_path(base::join_path(fs::current_directory(), self)) };
            const std::vector<std::string> me { absolute };
            return hang(recorded["hang"], me);
        }
        oldProtocol = recorded.is_object() && recorded.value("oldProtocol", false);
    }
    if (arguments.size() == 1 && arguments[0] == "--protocol-version") return protocol_version(oldProtocol);
    // 2026.8.8.4 has no `emit build-database` at all; it falls through to "not simulated" below,
    // the way an unknown subcommand of a real old mcpp would (the server's own protocol-version
    // check keeps it from ever asking in the first place).
    if (!oldProtocol && arguments.size() >= 2 && arguments[0] == "emit" && arguments[1] == "build-database") {
        return emit_build_database(std::span<const std::string> { arguments }.subspan(2));
    }
    if (!arguments.empty() && arguments[0] == "--version") {
        std::println("mcpp {}", VERSION);
        return 0;
    }
    // `mcpp build --configure-only`: the server's L2 fallback once mcpp advertises no
    // mcpp.build-database (project/mcpp.cpp `load_mcpp`). 2026.8.8.4 has no --configure-only
    // either, so the fallback fails too, and reading the project's own compile_commands.json is
    // what is left to exercise.
    if (oldProtocol && !arguments.empty() && arguments[0] == "build"
        && std::ranges::find(arguments, std::string { "--configure-only" }) != arguments.end()) {
        std::println(std::cerr, "error: unknown option: --configure-only");
        return 2;
    }
    // Anything else, notably a modern `build --configure-only`, is what the consumer must not
    // need. A build leaves the mark a real one would, a compile_commands.json in the project, so
    // that a fixture's workspace-unchanged check sees it was run.
    if (!arguments.empty() && arguments[0] == "build") (void)fs::write_file(base::join_path(fs::current_directory(), "compile_commands.json"), "[]\n");
    std::println(std::cerr, "mcppls-mock-mcpp: '{}' is not simulated", base::join(arguments, " "));
    return arguments.empty() ? 2 : 127;
}
