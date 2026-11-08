// The conformance fixtures as cases (conformance spec, tests/cts/README.md): each object below is
// one case, a fixture of fixtures/ run by `mcppls-conformance run` as a client of the server, and
// states where it runs (`on`), what it holds (`machine`, for a fixture whose checks bound how long
// an answer takes) and, by its name, the client it is: NAME@polling declares no dynamic
// registration of watched files, NAME@plain no `experimental.cxxModules`, NAME@vscode, @neovim and
// @zed what that editor sends. Every failed check is a failure of the case, named by its id.
//
// The environment names what the cases run against; CI's conformance job sets it:
//   MCPPLS_CTS_SERVER    the server under test            (default payload/bin/mcppls.okb)
//   MCPPLS_CTS_PAYLOAD   its payload                      (default payload)
//   MCPPLS_CTS_RUNNER    mcppls-conformance               (default: beside this program)
//   MCPPLS_CTS_FIXTURES  the fixtures                     (default tests/cts/fixtures)
//   MCPPLS_CTS_TIMEOUT   seconds each check may take      (default 180)
//   MCPPLS_CTS_MSVC_ENV  a developer environment (NAME=value lines) for the fixtures that build with MSVC
//   MCPPLS_CTS_MEASURES  a directory each case's measures are written to
//   MCPPLS_CTS_BUNDLES   a directory the diagnostic bundles the bundle checks exported are left in
//   MCPPLS_CTS_EVIDENCE  a directory a fixture that keeps evidence keeps its workspace and cache in
//   MCPPLS_CTS_VERBOSE   1: the runner prints the server's log (a failed case's diagnosis sets it)
import std;
import nlohmann.json;
import mcpplibs.os;
import mcpplibs.mtest;
import mcpplibs.mtest.e2e;
import mcpplibs.mtest.manifest;

using namespace mcpplibs::mtest;
namespace os = mcpplibs::os;
using json = nlohmann::json;

namespace {

// The platforms a case runs on, as tags `on:<platform>` a CI row selects by (`mtest run --tag`).
// A fixture is left off a platform whose runners lack what it builds with: MSVC (win32-x64 only),
// GCC 16, xmake and Qt (linux-x64), a mcpp toolchain store (none on linux-arm64, which takes
// distribution clang), or a POSIX path it bakes in (generated-module* off win32-x64).
struct on {
    std::vector<std::string_view> platforms;
    on(std::initializer_list<std::string_view> names) : platforms(names) {}
};
void apply(attributes& a, on o) {
    for (const auto p : o.platforms)
        a.tags.push_back(std::format("on:{}", p));
}

enum class client { own, polling, plain, vscode, neovim, zed };

struct options {
    client as = client::own;
    bool evidence = false;  // its workspace and cache are kept for the record (MCPPLS_CTS_EVIDENCE)
};

std::string env_or(const char* name, std::string_view fallback) {
    auto v = os::env::get(name);
    return v && !v->empty() ? *v : std::string(fallback);
}

std::string runner() {
    if (auto v = os::env::get("MCPPLS_CTS_RUNNER"); v && !v->empty())
        return *v;
    const auto self = self_command();
    const std::string exe = os::host() == os::platform::windows ? ".exe" : "";
    if (!self.empty()) {
        const auto beside = os::fs::from_utf8(self.back()).parent_path() / ("mcppls-conformance" + exe);
        if (std::filesystem::exists(beside))
            return os::fs::utf8(beside);
    }
    return e2e::program("mcppls-conformance");
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

// The failures a run reports: each failed check from its measures, then each other `FAIL` line
// (the handshake, a requirement, a budget of the whole run).
void judge(const e2e::outcome& r, const std::filesystem::path& measure, std::string_view fixture) {
    expect(fatal(r.started)) << "mcppls-conformance did not start";
    std::set<std::string> named;
    if (auto text = os::fs::read(measure)) {
        const auto j = json::parse(*text, nullptr, false);
        for (const auto& c : j.is_object() ? j.value("checks", json::array()) : json::array()) {
            if (c.value("ok", true) || c.value("optional", false))
                continue;
            named.insert(c.value("id", ""));
            expect(false) << std::format("{} {} {}: {}", fixture, c.value("id", ""), c.value("kind", ""),
                                         c.value("detail", ""));
        }
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

// Runs fixture `fixture` of the fixtures as the case does.
void conformance(std::string_view fixture, options o = {}) {
    const auto id = current_case();
    std::vector<std::string> argv{runner(), "run",
                                  "--server", env_or("MCPPLS_CTS_SERVER", "payload/bin/mcppls.okb"),
                                  "--payload", env_or("MCPPLS_CTS_PAYLOAD", "payload"),
                                  "--fixture", os::fs::utf8(os::fs::from_utf8(env_or("MCPPLS_CTS_FIXTURES", "tests/cts/fixtures")) / std::string(fixture)),
                                  "--timeout", env_or("MCPPLS_CTS_TIMEOUT", "180")};
    switch (o.as) {
        case client::own: break;
        case client::polling: argv.push_back("--no-dynamic-watch"); break;
        case client::plain: argv.insert(argv.end(), {"--client", "plain"}); break;
        case client::vscode: argv.insert(argv.end(), {"--client", "vscode"}); break;
        case client::neovim: argv.insert(argv.end(), {"--client", "neovim"}); break;
        case client::zed: argv.insert(argv.end(), {"--client", "zed"}); break;
    }
    if (const auto msvc = env_or("MCPPLS_CTS_MSVC_ENV", ""); !msvc.empty())
        argv.insert(argv.end(), {"--msvc-env", msvc});
    if (const auto bundles = env_or("MCPPLS_CTS_BUNDLES", ""); !bundles.empty())
        argv.insert(argv.end(), {"--keep-bundles", bundles});
    const bool verbose = env_or("MCPPLS_CTS_VERBOSE", "") == "1";
    if (verbose)
        argv.push_back("--verbose");
    if (const auto evidence = env_or("MCPPLS_CTS_EVIDENCE", ""); o.evidence && !evidence.empty()) {
        // A diagnostic run keeps its own, beside the first run's.
        const auto dir = os::fs::from_utf8(evidence) / (file_part(id) + (verbose ? "-diagnostic" : ""));
        argv.insert(argv.end(), {"--workspace-dir", os::fs::utf8(dir / "workspace"), "--cache-dir", os::fs::utf8(dir / "cache")});
    }
    auto measure = temp_dir() / "measure.json";
    if (const auto measures = env_or("MCPPLS_CTS_MEASURES", ""); !measures.empty()) {
        std::filesystem::create_directories(os::fs::from_utf8(measures));
        measure = os::fs::from_utf8(measures) / (file_part(id) + ".json");
    }
    argv.insert(argv.end(), {"--measure", os::fs::utf8(measure)});
    judge(e2e::run({.argv = argv, .timeout = std::chrono::minutes{40}}), measure, fixture);
}

client client_of(std::string_view variant) {
    if (variant == "polling") return client::polling;
    if (variant == "plain") return client::plain;
    if (variant == "vscode") return client::vscode;
    if (variant == "neovim") return client::neovim;
    if (variant == "zed") return client::zed;
    return client::own;
}

template <class... A>
constexpr bool ends_with_body() {
    if constexpr (sizeof...(A) == 0)
        return false;
    else
        return std::invocable<std::remove_cvref_t<std::tuple_element_t<sizeof...(A) - 1, std::tuple<A...>>>&>;
}

// A fixture as a case. Its body, unless it states one, runs the fixture its name names as the
// client its variant names; a failing case runs once more with the server's log (diagnose).
class Fixture : public CTS {
public:
    template <class... A>
    requires(!ends_with_body<A...>())
    Fixture(name_here name, A&&... args)
        : CTS(name, server_log(), std::forward<A>(args)..., [id = std::string(name.name)] {
              const auto at = id.find('@');
              conformance(std::string_view(id).substr(0, at),
                          {.as = at == std::string::npos ? client::own : client_of(std::string_view(id).substr(at + 1))});
          }) {}
    template <class... A>
    requires(ends_with_body<A...>())
    Fixture(name_here name, A&&... args) : CTS(name, server_log(), std::forward<A>(args)...) {}

private:
    static diagnose server_log() {
        return {.env = {{"MCPPLS_CTS_VERBOSE", "1"}, {"MCPPLS_CTS_TIMEOUT", "60"}}};
    }
};

}  // namespace

namespace cases {

Fixture build_discovery_off{"build-discovery-off", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture build_program_broken{"build-program-broken", on{"linux-x64", "darwin-arm64"}};
Fixture build_program_gated{"build-program-gated", on{"linux-x64", "darwin-arm64"}};
Fixture build_program_manual{"build-program-manual", on{"linux-x64", "darwin-arm64"}};
Fixture cache_budget{"cache-budget", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture clangd_cannot_load{"clangd-cannot-load", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture clangd_crash_closure{"clangd-crash-closure", on{"linux-x64", "linux-arm64", "darwin-arm64"}, resource{"machine"}};
Fixture clangd_crash_context{"clangd-crash-context", on{"linux-x64", "linux-arm64", "darwin-arm64"}, resource{"machine"}};
Fixture clangd_exit_strike{"clangd-exit-strike", on{"linux-x64", "linux-arm64", "darwin-arm64"}, resource{"machine"}};
Fixture cmake_clang{"cmake-clang", on{"linux-x64"}};
Fixture cmake_clang_bdb{"cmake-clang-bdb", on{"linux-x64"}};
Fixture cmake_clang_cl{"cmake-clang-cl", on{"win32-x64"}};
Fixture cmake_clangxx_msvc{"cmake-clangxx-msvc", on{"win32-x64"}};
Fixture cmake_fetchcontent_offline{"cmake-fetchcontent-offline", on{"linux-x64"}};
Fixture cmake_msvc{"cmake-msvc", on{"win32-x64"}};
Fixture cmake_msvc_bdb{"cmake-msvc-bdb", on{"win32-x64"}};
Fixture cmake_msvc_std{"cmake-msvc-std", on{"win32-x64"}};
Fixture compdb_clang_cl_std{"compdb-clang-cl-std", on{"win32-x64"}};
Fixture compdb_clangxx_msvc_std{"compdb-clangxx-msvc-std", on{"win32-x64"}};
Fixture compdb_lto_msvc{"compdb-lto-msvc", on{"linux-x64"}};
Fixture compdb_midwrite{"compdb-midwrite", on{"linux-x64"}};
Fixture compdb_mixed_standards{"compdb-mixed-standards", on{"linux-x64"}};
Fixture compdb_rejected_command{"compdb-rejected-command", on{"linux-x64"}};
Fixture completion_keywords{"completion-keywords", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture completion_templates{"completion-templates", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture diagnostic_bundle{"diagnostic-bundle", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture engine_none{"engine-none", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture failure_at_base{"failure-at-base", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture failure_at_base_neovim{"failure-at-base@neovim", on{"linux-x64"}, resource{"machine"}};
Fixture failure_at_base_vscode{"failure-at-base@vscode", on{"linux-x64", "win32-x64"}, resource{"machine"}};
Fixture failure_at_base_zed{"failure-at-base@zed", on{"darwin-arm64"}, resource{"machine"}};
Fixture format_explicit{"format-explicit", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture generated_module{"generated-module", on{"linux-x64", "darwin-arm64"}};
Fixture generated_module_vscode{"generated-module@vscode", on{"linux-x64"}};
Fixture generated_module_negotiated{"generated-module-negotiated", on{"linux-x64", "darwin-arm64"}};
Fixture generated_module_old_mcpp{"generated-module-old-mcpp", on{"linux-x64", "darwin-arm64"}};
Fixture generated_module_old_mcpp_neovim{"generated-module-old-mcpp@neovim", on{"linux-x64"}};
Fixture include_cleaner_modules{"include-cleaner-modules", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture inferred{"inferred", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture inferred_bom{"inferred-bom", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture inferred_cxx26{"inferred-cxx26", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture inferred_msvc{"inferred-msvc", on{"win32-x64"}};
Fixture kit_openkal{"kit-openkal", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_all_cppm{"mcpp-all-cppm", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_autosave_import_typing{"mcpp-autosave-import-typing", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture mcpp_build_programs{"mcpp-build-programs", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_build_programs_cross{"mcpp-build-programs-cross", on{"linux-x64"}};
Fixture mcpp_emit{"mcpp-emit", on{"linux-x64"}};
Fixture mcpp_emit_broken{"mcpp-emit-broken", on{"linux-x64"}};
Fixture mcpp_emit_edits{"mcpp-emit-edits", on{"linux-x64"}};
Fixture mcpp_emit_hang{"mcpp-emit-hang", on{"linux-x64", "win32-x64"}};
Fixture mcpp_emit_needs_download{"mcpp-emit-needs-download", on{"linux-x64"}};
Fixture mcpp_emit_package_std{"mcpp-emit-package-std", on{"linux-x64"}};
Fixture mcpp_emit_partial{"mcpp-emit-partial", on{"linux-x64"}};
Fixture mcpp_emit_partial_download{"mcpp-emit-partial-download", on{"linux-x64"}};
Fixture mcpp_emit_provisioned{"mcpp-emit-provisioned", on{"linux-x64"}};
Fixture mcpp_emit_unavailable{"mcpp-emit-unavailable", on{"linux-x64"}};
Fixture mcpp_emit_wait{"mcpp-emit-wait", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_emit_watch{"mcpp-emit-watch", on{"linux-x64"}};
Fixture mcpp_emit_watch_polling{"mcpp-emit-watch@polling", on{"linux-x64"}};
Fixture mcpp_gcc{"mcpp-gcc", on{"linux-x64"}};
Fixture mcpp_gcc_plain{"mcpp-gcc@plain", on{"linux-x64"}};
Fixture mcpp_llvm{"mcpp-llvm", on{"linux-x64", "darwin-arm64"}};
Fixture mcpp_llvm_msvc{"mcpp-llvm-msvc", on{"win32-x64"}};
Fixture mcpp_msvc{"mcpp-msvc", on{"win32-x64"}};
Fixture mcpp_partition_definition{"mcpp-partition-definition", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_polling_watch_inputs_polling{"mcpp-polling-watch-inputs@polling", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_producer_file_identity{"mcpp-producer-file-identity", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_producer_identity{"mcpp-producer-identity", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_producer_identity_polling{"mcpp-producer-identity@polling", on{"linux-x64"}};
Fixture mcpp_producer_relative_path{"mcpp-producer-relative-path", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_rules_generated{"mcpp-rules-generated", on{"linux-x64"}};
Fixture mcpp_split{"mcpp-split", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_split_gcc{"mcpp-split-gcc", on{"linux-x64"}};
Fixture mcpp_split_msvc{"mcpp-split-msvc", on{"win32-x64"}};
Fixture mcpp_target_auto_fallback{"mcpp-target-auto-fallback", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_build_tool_off{"mcpp-target-build-tool-off", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_discovery_off{"mcpp-target-discovery-off", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_explicit{"mcpp-target-explicit", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_fallback_mismatch{"mcpp-target-fallback-mismatch", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_fallback_preserve{"mcpp-target-fallback-preserve", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_invalid{"mcpp-target-invalid", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_live{"mcpp-target-live", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_mismatch{"mcpp-target-mismatch", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_mismatch_cold{"mcpp-target-mismatch-cold", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_project{"mcpp-target-project", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_provider_excluded{"mcpp-target-provider-excluded", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_race{"mcpp-target-race", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_selection{"mcpp-target-selection", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_selection_polling{"mcpp-target-selection@polling", on{"linux-x64"}};
Fixture mcpp_target_windows{"mcpp-target-windows", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_target_windows_real{"mcpp-target-windows-real", on{"win32-x64"}, [] { conformance("mcpp-target-windows-real", {.evidence = true}); }};
Fixture mcpp_watch{"mcpp-watch", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_watch_polling{"mcpp-watch@polling", on{"linux-x64"}};
Fixture mcpp_watched_source_structure{"mcpp-watched-source-structure", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mingw{"mingw", on{"linux-x64", "win32-x64"}};
Fixture module_edit_autosave{"module-edit-autosave", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture module_fails_in_import{"module-fails-in-import", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture module_faults{"module-faults", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture module_faults_zed{"module-faults@zed", on{"linux-x64"}, resource{"machine"}};
Fixture multi_root{"multi-root", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture no_modules{"no-modules", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture partial_scan_standins{"partial-scan-standins", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture payload_corrupt{"payload-corrupt", on{"linux-x64", "linux-arm64"}};
Fixture preparation_after_idle{"preparation-after-idle", on{"linux-x64", "win32-x64"}};
Fixture provisional_no_prime{"provisional-no-prime", on{"linux-x64", "darwin-arm64"}};
Fixture reset_cache{"reset-cache", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture s1_two_sets{"s1-two-sets", on{"linux-x64"}};
Fixture save_plan{"save-plan", on{"linux-x64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture tidy_const_views{"tidy-const-views", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture typing_autosave{"typing-autosave", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture typing_import{"typing-import", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture typing_import_spin{"typing-import-spin", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture typing_pause{"typing-pause", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture untrusted{"untrusted", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture ux_modules_qt{"ux-modules-qt", on{"linux-x64"}, resource{"machine"}};
Fixture ux_modules_windows{"ux-modules-windows", on{"win32-x64"}, resource{"machine"}};
Fixture verify_changes{"verify-changes", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture watch_polling{"watch-polling", on{"linux-x64", "linux-arm64"}, [] { conformance("watch-polling", {.as = client::polling}); }};
Fixture workaround_canaries{"workaround-canaries", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture xmake_basic{"xmake-basic", on{"linux-x64"}};
Fixture xmake_late_config{"xmake-late-config", on{"linux-x64"}};
Fixture xmake_needs_download{"xmake-needs-download", on{"linux-x64"}};
Fixture xmake_no_standard{"xmake-no-standard", on{"linux-x64"}};
Fixture xmake_shared_tmp{"xmake-shared-tmp", on{"linux-x64"}};
Fixture xmake_stale_state{"xmake-stale-state", on{"linux-x64"}, resource{"machine"}};
Fixture xmake_target_then_mode{"xmake-target-then-mode", on{"linux-x64"}};
Fixture xmake_user_mode{"xmake-user-mode", on{"linux-x64"}};

}  // namespace cases

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

// The declarations agree with the fixtures: every case names a fixture there is, and a fixture
// whose checks bound an answer's time holds the machine. Read from this file's text (the static
// manifest), as `mtest list --static` reads it.
CTS declarations{"the cases and the fixtures agree", on{"linux-x64"}, [] {
    const auto source = os::fs::read(os::fs::from_utf8("tests/cts/conformance.cpp"));
    if (!source)
        skip("tests/cts/conformance.cpp is not in the working directory");
    const auto fixtures = os::fs::from_utf8(env_or("MCPPLS_CTS_FIXTURES", "tests/cts/fixtures"));
    std::size_t seen = 0;
    for (const auto& c : mcpplibs::mtest::manifest::scan(*source)) {
        if (c.kind != "cts" || c.id.find('/') != std::string::npos || c.id == current_case())
            continue;
        ++seen;
        const auto fixture = c.id.substr(0, c.id.find('@'));
        const auto scenario = os::fs::read(fixtures / fixture / "scenario.json");
        expect(scenario.has_value()) << c.id << " names no fixture of " << os::fs::utf8(fixtures);
        if (!scenario)
            continue;
        const bool holds = std::ranges::find(c.resources, "machine") != c.resources.end();
        expect(holds || !times_answers(json::parse(*scenario, nullptr, false)))
            << c.id << " bounds how long an answer takes and holds no machine (resource{\"machine\"})";
    }
    expect(gt(seen, std::size_t(100))) << "the manifest read " << seen << " cases";
}};

}  // namespace

int main(int argc, char** argv) {
    return mcpplibs::mtest::run(argc, argv);
}
