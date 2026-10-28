module mcppls.project.mcpp;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.log;
import mcppls.base.version;
import mcppls.spec.database;
import mcppls.spec.discovery;
import mcppls.project.detect;
import mcppls.project.compdb;
import mcppls.project.infer;
import mcppls.project.provider;
import mcppls.project.target;
import mcppls.project.boundary;
import mcpplibs.os;
import mcppls.base.home;
import mcppls.toolrun;
import mcppls.toolenv;

namespace os = mcpplibs::os;

namespace mcppls::project {

std::string mcpp_package_name(std::string_view manifestText) {
    bool inPackage { false };
    for (auto line : base::split_lines(manifestText)) {
        line = base::trim(line);
        if (line.starts_with('[')) {
            inPackage = line == "[package]";
            continue;
        }
        if (!inPackage || !line.starts_with("name")) continue;
        const std::size_t equals { line.find('=') };
        if (equals == std::string_view::npos || base::trim(line.substr(0, equals)) != "name") continue;
        std::string_view value { base::trim(line.substr(equals + 1)) };
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) {
            const std::size_t close { value.find(value.front(), 1) };
            if (close != std::string_view::npos) return std::string { value.substr(1, close - 1) };
        }
    }
    return {};
}

namespace {

// The leading run of digits at `text[i]`, as a number, and how far it advanced; 0 and no advance
// for a segment that has none (a pre-release suffix, an empty segment).
std::pair<std::uint64_t, std::size_t> leading_number(std::string_view text) {
    std::size_t end { 0 };
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
    if (end == 0) return { 0, 0 };
    std::uint64_t value { 0 };
    for (std::size_t i { 0 }; i < end && i < 19; ++i) value = value * 10 + static_cast<std::uint64_t>(text[i] - '0');
    return { value, end };
}

} // namespace

bool mcpp_version_less(std::string_view left, std::string_view right) {
    std::size_t li { 0 };
    std::size_t ri { 0 };
    while (li < left.size() || ri < right.size()) {
        const auto [lv, ladvance] = leading_number(left.substr(li));
        const auto [rv, radvance] = leading_number(right.substr(ri));
        if (lv != rv) return lv < rv;
        li += ladvance;
        ri += radvance;
        // Skip one non-digit separator ('.', '-', ...) on whichever side still has one, so "2026.9"
        // and "2026.9.0" compare equal in their shared segments and neither side gets stuck.
        if (li < left.size() && (left[li] < '0' || left[li] > '9')) ++li;
        if (ri < right.size() && (right[ri] < '0' || right[ri] > '9')) ++ri;
        if (ladvance == 0 && radvance == 0) break;
    }
    return false;
}

std::vector<std::string> other_mcpp_executables(std::string_view resolved, std::string_view homeDirectory) {
    std::error_code ignored;
    std::vector<std::string> found;
    for (std::string_view store : { ".xlings/data/xpkgs", ".mcpp/registry/data/xpkgs" }) {
        const std::string packageDirectory { base::join_path(std::string { homeDirectory }, std::format("{}/xim-x-mcpp", store)) };
        auto versions = base::children(packageDirectory);
        std::ranges::sort(versions, [](const std::string& a, const std::string& b) { return mcpp_version_less(base::file_name(b), base::file_name(a)); });
        for (const auto& version : versions) {
            const std::string candidate { base::join_path(version, "bin/mcpp") + std::string { os::conventions().exe_suffix } };
            if (!std::filesystem::is_regular_file(candidate, ignored)) continue;
            if (base::same_path(candidate, resolved)) continue;
            if (std::ranges::find_if(found, [&](const std::string& existing) { return base::same_path(existing, candidate); }) != found.end()) continue;
            found.push_back(candidate);
        }
    }
    return found;
}

std::vector<CompileCommand> mcpp_standard_units(std::span<const CompileCommand> commands) {
    static constexpr std::string_view STAGED { "-fmodule-file=std=" };
    std::string stagedBmi;
    for (const auto& command : commands) {
        const auto argument = std::ranges::find_if(command.arguments, [](const std::string& word) { return word.starts_with(STAGED); });
        if (argument == command.arguments.end()) continue;
        stagedBmi = argument->substr(STAGED.size());
        if (!base::is_absolute_path(stagedBmi)) stagedBmi = base::join_path(command.directory, stagedBmi);
        break;
    }
    if (stagedBmi.empty()) return {};
    // <build directory>/pcm.cache/std.pcm, staged by `build pcm.cache/std.pcm : stage_file <cache>/pcm.cache/std.pcm`.
    const std::string buildDirectory { base::parent_path(base::parent_path(stagedBmi)) };
    const auto ninja = os::fs::read(base::join_path(buildDirectory, "build.ninja"));
    if (!ninja) return {};
    static constexpr std::string_view RULE { "build pcm.cache/std.pcm : stage_file " };
    std::string cachedBmi;
    for (const auto line : base::split_lines(*ninja)) {
        if (!line.starts_with(RULE)) continue;
        const std::string_view rest { line.substr(RULE.size()) };
        cachedBmi = std::string { base::trim(rest.substr(0, rest.find_first_of(" |"))) };
        break;
    }
    if (cachedBmi.empty()) return {};
    const std::string recordPath { base::join_path(base::parent_path(base::parent_path(cachedBmi)), "std-module.json") };
    const auto recordText = os::fs::read(recordPath);
    if (!recordText) return {};
    const nlohmann::json record = nlohmann::json::parse(*recordText, nullptr, false);
    if (!record.is_object() || record.value("schema", 0) != 1) return {};

    std::vector<CompileCommand> units;
    auto add = [&](const char* sourceKey, const char* commandsKey) {
        const std::string source { record.value(sourceKey, std::string {}) };
        if (source.empty() || !record.contains(commandsKey) || !record[commandsKey].is_array()) return;
        for (const auto& text : record[commandsKey]) {
            if (!text.is_string()) continue;
            // `cd '<dir>' && env NAME=value... '<compiler>' <arguments> --precompile '<source>' -o '<bmi>' 2>&1`
            const auto words = split_command(text.get<std::string>(), CommandSyntax::posix);
            if (std::ranges::find(words, std::string_view { "--precompile" }) == words.end()) continue;
            CompileCommand unit;
            unit.directory = base::parent_path(recordPath);
            unit.file = source;
            std::size_t i { 0 };
            if (words.size() > 3 && words[0] == "cd" && words[2] == "&&") {
                unit.directory = words[1];
                i = 3;
            }
            if (i < words.size() && words[i] == "env") {
                for (++i; i < words.size() && words[i].find('=') != std::string::npos && !words[i].starts_with('-'); ++i) {}
            }
            for (; i < words.size(); ++i) {
                const std::string& word { words[i] };
                if (word == "2>&1" || word == "--precompile") continue;
                if (word == "-o") {
                    ++i;
                    continue;
                }
                if (word.starts_with("-fmodule-file=")) continue;
                unit.arguments.push_back(word);
            }
            if (unit.arguments.empty()) return;
            if (std::ranges::find(unit.arguments, source) == unit.arguments.end()) unit.arguments.push_back(source);
            units.push_back(std::move(unit));
            return;
        }
    };
    add("std_module_source", "std_build_commands");
    add("std_compat_source", "std_compat_build_commands");
    return units;
}

namespace {

// What a failed command said, on one line: its standard error, or its output when that is empty.
// An mcpp that a project's .xlings.json selects but that is not installed fails before it starts,
// and xlings says so there.
std::string explanation(const mcpplibs::os::process::bounded_output& result) {
    std::string text;
    const std::string& said { base::trim(result.err).empty() ? result.out : result.err };
    for (const std::string_view line : base::split_lines(said)) {
        const std::string_view trimmed { base::trim(line) };
        if (trimmed.empty()) continue;
        if (!text.empty()) text += "; ";
        for (std::size_t i { 0 }; i < trimmed.size(); ++i) {
            if (trimmed[i] == ' ' && i > 0 && trimmed[i - 1] == ' ') continue;   // alignment
            text += trimmed[i];
        }
    }
    if (text.size() > 400) text = text.substr(0, 400) + "...";
    return text;
}

// The mcpp executables and roots whose mcpp said it produces build databases, with the version it said
// it is. Asking starts a process (0.3 s through the xlings launcher) on every reload a watched input
// causes, so a yes is kept until emit build-database fails. A no is not kept: the upgrade that
// producer-writes-project asks for would go unnoticed until the server restarted. The version is kept
// with the yes (0.0.13 W7): without it every describe after the first reported no version, and a report,
// or the hint that names the mcpp a build.mcpp needs, could not say which mcpp was in use.
class ProducerAnswers {
public:
    // The version remembered for `key`, when a yes is.
    std::optional<std::string> known(const std::string& key) {
        std::lock_guard lock { mutex_ };
        const auto it = answers_.find(key);
        if (it == answers_.end()) return std::nullopt;
        if (std::chrono::steady_clock::now() - it->second.checked > std::chrono::seconds { 30 }) {
            answers_.erase(it);
            return std::nullopt;
        }
        return it->second.version;
    }
    void remember(std::string key, std::string version) {
        std::lock_guard lock { mutex_ };
        if (const auto existing = answers_.find(key); existing != answers_.end()) {
            existing->second.version = std::move(version); // emit identity does not extend capability-query freshness
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(answers_, [&](const auto& entry) { return now - entry.second.checked > std::chrono::seconds { 30 }; });
        if (answers_.size() >= 128) {
            const auto oldest = std::ranges::min_element(answers_, {}, [](const auto& entry) { return entry.second.checked; });
            answers_.erase(oldest);
        }
        answers_.insert_or_assign(std::move(key), Answer { std::move(version), now });
    }
    void forget(const std::string& key) {
        std::lock_guard lock { mutex_ };
        answers_.erase(key);
    }

private:
    std::mutex mutex_;
    struct Answer { std::string version; std::chrono::steady_clock::time_point checked; };
    std::map<std::string, Answer, std::less<>> answers_;
};

ProducerAnswers& producer_answers() {
    static ProducerAnswers answers;
    return answers;
}

// `mcpp --protocol-version` in the project, whose .xlings.json may select another mcpp than the one on
// PATH: whether that mcpp advertises the kind and runs the command without writing into the project.
// `version` is set when it said which it is. `reachable`, when given, is set once this mcpp answered
// the command at all (exit 0) -- distinct from the kind being missing, since only a real, running but
// old mcpp is worth negotiating a substitute for (real-project plan RP2.1): a project whose .xlings.json names
// an mcpp that is not installed at all answers nothing, and substituting another mcpp for it would
// hide that fact instead of reporting it, which mcpp-emit-unavailable exists to keep visible.
bool produces_build_databases(const std::string& mcpp, const Detection& detection, const ProviderContext& context, std::string& version,
                              bool* reachable = nullptr) {
    auto answered = mcppls::toolrun::run({
        .program = mcpp,
        .arguments = { "--protocol-version" },
        .workDirectory = detection.root,
        .purpose = "protocol",
        .root = context.rootKey,
        .network = context.offline ? mcppls::toolrun::Network::offline : mcppls::toolrun::Network::allowed,
        .bounds = mcpplibs::os::process::bounds { .hard = std::chrono::seconds { 20 } },
        .environmentWait = context.environmentWait,
    });
    if (!answered || answered->timed_out) return false;
    if (!answered->status.success()) {
        base::log::info("mcpp --protocol-version failed ({}): {}", toolrun::exit_code(answered->status), explanation(*answered));
        base::log::info("  a build database needs mcpp >= {}; without one the project is read from "
                        "compile_commands.json if there is one", base::MINIMUM_MCPP_VERSION);
        return false;
    }
    if (reachable != nullptr) *reachable = true;
    const nlohmann::json described = nlohmann::json::parse(answered->out, nullptr, false);
    if (described.is_object()) {
        if (const auto producer = described.find("mcpp"); producer != described.end() && producer->is_object()) {
            version = producer->value("version", std::string {});
        }
    }
    auto protocol = spec::parse_producer_protocol(answered->out);
    if (!protocol || !protocol->kinds.contains("mcpp.build-database")) {
        base::log::info("mcpp{}{} does not produce build databases (no mcpp.build-database kind); using its compile database",
                        version.empty() ? "" : " ", version);
        base::log::info("  the kind arrived in mcpp {}; upgrading gives exact module information",
                        base::MINIMUM_MCPP_VERSION);
        return false;
    }
    if (const auto effects = protocol->commandEffects.find("emit build-database");
        effects != protocol->commandEffects.end() && !spec::effects_acceptable(effects->second)) {
        base::log::warning("mcpp emit build-database declares that it writes into the project; not running it");
        return false;
    }
    return true;
}

void check_requested_target(InferredDatabase& database, const Detection& detection, const ProviderContext& context, bool fallback = false) {
    const bool unapplied { fallback && !context.runBuildTool };
    const std::string unverifiedCode { unapplied ? "mcpp-target-unapplied" : "mcpp-target-unverified" };
    const std::string mismatchCode { unapplied ? "mcpp-target-unapplied" : "mcpp-target-mismatch" };
    if (!context.targetSelection.requestedTarget.empty()) {
        bool projectSet { false };
        const ProjectBoundaries boundaries { detection.root };
        for (const auto& set : database.database.sets) {
            if (spec::is_build_program_set(set) || set.units.empty()) continue;
            const bool local { std::ranges::any_of(set.units, [&](const spec::TranslationUnit& unit) {
                const std::string source { spec::absolute_source(unit) };
                return boundaries.owns(source);
            }) };
            if (!local) continue;
            projectSet = true;
            const auto* toolchain = spec::find_toolchain(database.database, set.toolchain);
            if (toolchain == nullptr || toolchain->target.empty()) {
                database.issues.emplace_back(unverifiedCode, "the producer did not identify the effective target of a project set");
            } else if (!mcpp_target_matches(context.targetSelection.requestedTarget, toolchain->target)) {
                database.issues.emplace_back(mismatchCode, std::format("requested target {} but project set {} uses {}",
                    context.targetSelection.requestedTarget, set.name, toolchain->target));
            }
            if (const auto facts = database.facts.find(set.toolchain); facts != database.facts.end()
                && !facts->second.toolchain.target.empty()
                && !mcpp_target_matches(context.targetSelection.requestedTarget, facts->second.toolchain.target)) {
                database.issues.emplace_back(mismatchCode, std::format("requested target {} but compiler commands for project set {} resolve to {}",
                    context.targetSelection.requestedTarget, set.name, facts->second.toolchain.target));
            }
        }
        if (!projectSet) database.issues.emplace_back(unverifiedCode, "the producer described no identifiable project sets for the requested target");
    }
}

// S2 0.2 single-document mode against mcpp's machine-output protocol (mcpp-community/mcpp#636):
// `mcpp --protocol-version` advertises the kind, `mcpp emit build-database --format json` answers it.
// nullopt when this mcpp cannot be asked; an mcpp that can be asked and fails says why, and nothing
// else is tried: configuring instead would write into the project and hide what mcpp reported.
std::optional<base::Result<InferredDatabase>> emit_build_database(const std::string& mcpp, const Detection& detection, const ProviderContext& context,
                                                                  std::string& version, bool* reachable = nullptr) {
    std::string key { std::format("{}\n{}\n{}", base::canonical_path(mcpp), detection.root,
                                  target_selection_key(context.targetSelection)) };
    // The path alone cannot identify a replaced binary, or a launcher whose project pin changed.
    for (const std::string& path : { mcpp, base::join_path(detection.root, ".xlings.json"),
                                     base::join_path(detection.root, "mcpp.toml"), base::join_path(detection.root, "mcpp.lock") }) {
        if (const auto stamp = base::file_stamp(path)) key += std::format("\n{}:{}:{}", path, stamp->size, static_cast<std::int64_t>(stamp->modified_ns));
        if (const auto identity = os::fs::identity_of(path)) key += std::format(":{}:{}", identity->volume, identity->index);
    }
    if (const auto remembered = producer_answers().known(key)) {
        version = *remembered;
        if (reachable != nullptr) *reachable = true;   // a "yes" was already remembered: it answered, or this key would not be known
    } else {
        if (!produces_build_databases(mcpp, detection, context, version, reachable)) return std::nullopt;
        producer_answers().remember(key, version);
    }
    std::vector<std::string> command { mcpp, "emit", "build-database", "--format", "json" };
    if (!context.targetSelection.requestedTarget.empty()) {
        command.push_back("--target");
        command.push_back(context.targetSelection.requestedTarget);
    }
    const spec::RunContext how {
        .purpose = "producer",
        .root = context.rootKey,
        .offline = context.offline,
        .hard = context.producerHard,
        .soft = context.producerSoft,
        .environmentWait = context.environmentWait,
        .onSoftDeadline = context.onSlow,
    };
    auto document = spec::run_database_command(command, detection.root, how);
    if (!document) {
        producer_answers().forget(key);
        // Needing a download is not a failure of the producer: it is the offline default meeting a
        // project whose dependencies are not here yet. The caller keeps its model and offers an action.
        if (document.error().code == spec::NEEDS_DOWNLOAD) {
            base::log::info("mcpp emit build-database needs a download: {}", document.error().message);
            return base::fail(document.error().code, document.error().message);
        }
        base::log::warning("mcpp emit build-database failed: {}", document.error().message);
        return base::fail(document.error().code, std::format("mcpp emit build-database failed: {}", document.error().message));
    }
    // A launcher may select a different installed mcpp without its own file or project pin changing.
    // The successful invocation's envelope identifies the program that actually answered.
    if (!document->producerVersion.empty()) {
        version = document->producerVersion;
        producer_answers().remember(key, version);
    }
    if (document->networkObserved && context.offline) {
        base::log::warning("mcpp reached the network on a run this server asked to be offline");
    }
    if (std::ranges::find(document->effects, std::string_view { "write-project" }) != document->effects.end()) {
        base::log::warning("mcpp emit build-database reports that it wrote into the project");
    }
    auto database = spec::from_json(document->database, detection.root);
    if (!database) {
        base::log::warning("mcpp's build database cannot be read: {}", database.error().message);
        return base::fail(database.error().code, std::format("mcpp's build database cannot be read: {}", database.error().message));
    }
    auto enriched = enrich_database(std::move(*database), context.scanner, context.prober);
    check_requested_target(enriched, detection, context);
    enriched.watch = std::move(document->watch);
    // S2 0.3.0 (S2-3.4-12, S2-3.4-13; mcpp-community/mcpp#699): a document with `data` and `error` diagnostics describes
    // everything but what they name. The rest is used, and each part mcpp could not describe is said, by its file.
    for (const auto& diagnostic : document->diagnostics) {
        if (diagnostic.severity != "error") continue;
        const std::string where { diagnostic.path.empty() ? std::string {} : std::format("{}: ", diagnostic.path) };
        base::log::warning("mcpp described the project in part: {}{}: {}", where, diagnostic.code, diagnostic.message);
        enriched.issues.emplace_back("producer-partial", std::format("mcpp could not describe {}{}: {}; the rest of the project is used",
                                                                       where.empty() ? std::string { "part of the project: " } : where, diagnostic.code,
                                                                       diagnostic.message));
        // B-8 (plan §3.4, §9.3 T9): a partial answer's missing member can itself be missing because
        // it needs a download offline forbids (mcpp #699 plans members independently, so one member's
        // MCPP_OFFLINE_DOWNLOAD_REQUIRED no longer fails the whole document the way it used to). Without
        // this the workspace only ever saw `producer-partial` here and never entered the needs-download
        // path, so nobody was ever asked to fetch the dependency; the partial model is kept either way.
        if (diagnostic.code == spec::OFFLINE_DOWNLOAD_REQUIRED) {
            enriched.issues.emplace_back(std::string { spec::NEEDS_DOWNLOAD }, diagnostic.message);
        }
        // 0.0.13 W3: a build program (build.mcpp) that did not compile or run. What it contributes to the targets -- their
        // flags, the units its rules generate -- is missing from this answer, which the workspace therefore does not take
        // in place of a complete one; mcpp's message goes onto the file.
        if (diagnostic.code == spec::BUILD_PROGRAM_FAILED) {
            // The file it is about on the message's first line (model.cpp makes it the issue's path).
            enriched.issues.emplace_back(std::string { spec::BUILD_PROGRAM_FAILED_ISSUE }, std::format("{}\n{}", diagnostic.path, diagnostic.message));
        }
    }
    return base::Result<InferredDatabase> { std::move(enriched) };
}

} // namespace

std::string mcpp_fallback_database(std::string_view root) {
    std::error_code ignored;
    const std::string project { root };
    // A relative path in a record is the project's: mcpp writes them absolute, and a hand-copied
    // tree keeps its own target/.
    const auto at_root = [&](const std::string& path) { return base::is_absolute_path(path) ? base::normalize_path(path) : base::join_path(project, path); };
    const auto json_of = [](const std::string& path) -> std::optional<nlohmann::json> {
        auto text = os::fs::read(path, 4 * 1024 * 1024);
        if (!text) return std::nullopt;
        auto parsed = nlohmann::json::parse(*text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
        return parsed;
    };
    if (const auto last = json_of(base::join_path(project, std::string { MCPP_LAST_RESULT }));
        last && last->value("kind", std::string {}) == "mcpp.last-result") {
        const auto data = last->find("data");
        if (data != last->end() && data->is_object() && data->contains("result") && (*data)["result"].is_string()) {
            if (const auto record = json_of(at_root((*data)["result"].get<std::string>()));
                record && record->value("kind", std::string {}) == "mcpp.result") {
                const auto recordData = record->find("data");
                if (recordData != record->end() && recordData->is_object() && recordData->contains("compileDatabase")
                    && (*recordData)["compileDatabase"].is_string()) {
                    const std::string database { at_root((*recordData)["compileDatabase"].get<std::string>()) };
                    if (std::filesystem::is_regular_file(database, ignored)) return database;
                }
            }
        }
    }
    const std::string rootCopy { base::join_path(project, "compile_commands.json") };
    return std::filesystem::is_regular_file(rootCopy, ignored) ? rootCopy : std::string {};
}

base::Result<InferredDatabase> load_mcpp(const Detection& detection, const ProviderContext& context) {
    std::vector<std::pair<std::string, std::string>> notices;
    std::string failure;   // why mcpp produced no compile database, in its own words
    if (context.targetSelection.mode == "explicit" && context.targetSelection.requestedTarget.empty()) {
        return base::fail("mcpp-target-invalid", "invalid mcppls.buildDiscovery.mcppTarget setting");
    }
    if (context.trusted && context.runBuildTool) {
        const std::string home { base::home_directory() };
        const std::vector<std::string> fallbacks { base::join_path(home, ".mcpp/bin/mcpp"), base::join_path(home, ".xlings/subos/current/bin/mcpp") };
        const std::string configured { context.mcppExecutable.empty() ? "mcpp" : context.mcppExecutable };
        const auto environment = mcppls::toolenv::get(context.environmentWait);
        std::string toolPath;
        for (const auto& variable : environment.variables) {
            const auto equals = variable.find('=');
            if (equals != std::string::npos && base::to_lower_ascii(variable.substr(0, equals)) == "path") {
                toolPath = variable.substr(equals + 1);
                break;
            }
        }
        // Resolve exactly the invocation used by toolrun, while preserving a launcher's basename.
        // A relative configured path belongs to the project, never the server's own working directory.
        std::optional<std::string> mcpp;
        if (base::is_absolute_path(configured) || configured.find_first_of("/\\") != std::string::npos) {
            const std::string path { base::join_path(detection.root, configured) };
            mcpp = toolrun::find_program(path, toolPath).value_or(path);
        } else {
            mcpp = toolrun::find_program(configured, toolPath);
            if (!mcpp && context.mcppExecutable.empty()) {
                for (const auto& fallback : fallbacks) {
                    if (auto found = toolrun::find_program(fallback, toolPath)) { mcpp = std::move(found); break; }
                }
            }
        }
        if (mcpp) {
            std::string version;
            bool reachable { false };
            context.producerUsed = *mcpp;
            auto emitted = emit_build_database(*mcpp, detection, context, version, &reachable);
            context.producerVersionUsed = version;
            if (emitted) return std::move(*emitted);
            // Producer negotiation (real-project plan RP2.1): the project's own mcpp answered but does not
            // advertise `mcpp.build-database` -- a real, working, old mcpp. Another mcpp installed on
            // the machine might advertise it, asked the same read-only, offline way as above, only to
            // describe the project; what the project itself builds with does not change. Newest
            // first, stopping at the first that answers. Nothing is negotiated when the project's own
            // mcpp did not even answer `--protocol-version` (not installed, wrong path, a broken
            // shim): that is a different, more direct problem, and substituting another mcpp for it
            // would hide the failure mcpp-emit-unavailable exists to keep visible, rather than fix it.
            for (const auto& candidate : reachable ? other_mcpp_executables(*mcpp, base::home_directory()) : std::vector<std::string> {}) {
                std::string candidateVersion;
                auto negotiated = emit_build_database(candidate, detection, context, candidateVersion);
                if (!negotiated || !*negotiated) continue;
                context.producerUsed = candidate;
                context.producerVersionUsed = candidateVersion;
                auto described = std::move(**negotiated);
                described.notices.emplace_back("producer-negotiated",
                    std::format("described by {} (the project pins {})",
                                candidateVersion.empty() ? candidate : std::format("mcpp {}", candidateVersion),
                                version.empty() ? std::format("{}, which has no emit build-database", *mcpp) : std::format("mcpp {}", version)));
                return base::Result<InferredDatabase> { std::move(described) };
            }
            // Neither the project's own mcpp nor another installed one can describe the build without
            // configuring it, which writes into the project.
            std::vector<std::string> configureArguments { "build", "--configure-only" };
            if (!context.targetSelection.requestedTarget.empty()) {
                configureArguments.push_back("--target");
                configureArguments.push_back(context.targetSelection.requestedTarget);
            }
            auto result = mcppls::toolrun::run({
                .program = *mcpp,
                .arguments = std::move(configureArguments),
                .workDirectory = detection.root,
                .purpose = "configure",
                .root = context.rootKey,
                .network = context.offline ? mcppls::toolrun::Network::offline : mcppls::toolrun::Network::allowed,
                .bounds = mcpplibs::os::process::bounds { .hard = context.configureTimeout },
                .soft = context.producerSoft,
                .environmentWait = context.environmentWait,
                .onSoftDeadline = context.onSlow,
            });
            if (!result) {
                base::log::warning("mcpp configure could not start: {}", result.error().message);
                failure = result.error().message;
            } else if (result->timed_out || !result->status.success()) {
                base::log::warning("mcpp build --configure-only failed ({}): {}", toolrun::exit_code(result->status), base::trim(result->err));
                failure = result->timed_out ? std::string { "mcpp build --configure-only timed out" } : explanation(*result);
            } else {
                notices.emplace_back("producer-writes-project",
                                     std::format("{} has no emit build-database, which mcpp 2026.9.15.1 added; mcpp build --configure-only "
                                                 "wrote compile_commands.json and target/ into the project",
                                                 version.empty() ? std::string { "this mcpp" } : std::format("mcpp {}", version)));
            }
        } else {
            base::log::info("mcpp was not found; using the compile database its last build left, if there is one");
            base::log::info("  mcpp >= {} gives exact module information (mcpp emit build-database)",
                            base::MINIMUM_MCPP_VERSION);
        }
    }
    // Asked after mcpp ran: `build --configure-only` above is what writes either of them.
    const std::string commandsPath { mcpp_fallback_database(detection.root) };
    if (commandsPath.empty()) {
        if (!context.trusted) return base::fail("mcpp-no-database", "the workspace is not trusted, so mcpp was not run");
        if (!context.runBuildTool) return base::fail("mcpp-no-database", "mcppls.buildTool is off, so mcpp was not run");
        return base::fail("mcpp-no-database", failure.empty() ? std::string { "mcpp did not produce compile_commands.json" }
                                                              : std::format("mcpp could not describe the project: {}", failure));
    }
    auto commands = read_compile_commands(commandsPath);
    if (!commands) return std::unexpected { commands.error() };
    // A package's std, which an mcpp before 2026.9.15.1 builds outside the compile database (W8).
    for (auto& unit : mcpp_standard_units(*commands)) {
        const bool listed { std::ranges::any_of(*commands, [&](const CompileCommand& command) { return base::same_path(command.file, unit.file); }) };
        if (!listed) {
            base::log::info("the standard library module {} comes from mcpp's std build record", unit.file);
            commands->push_back(std::move(unit));
        }
    }
    std::string name { std::string { base::file_name(detection.root) } };
    if (auto manifest = os::fs::read(detection.manifest)) {
        if (std::string package { mcpp_package_name(*manifest) }; !package.empty()) name = package;
    }
    auto database = database_from_commands(*commands, name, context.scanner, context.prober);
    database.database.generator = spec::Generator { "mcppls", "mcpp compile_commands.json" };
    database.notices = std::move(notices);
    check_requested_target(database, detection, context, true);
    return database;
}

std::optional<Claim> McppProvider::detect(std::string_view root) const {
    std::error_code ignored;
    const std::string manifest { base::join_path(std::string { root }, "mcpp.toml") };
    if (!std::filesystem::is_regular_file(manifest, ignored)) return std::nullopt;
    Claim claim;
    claim.provider = "mcpp";
    claim.confidence = 100;
    claim.root = std::string { root };
    claim.manifest = manifest;
    if (std::string commands { mcpp_fallback_database(root) }; !commands.empty()) claim.compileCommands = std::move(commands);
    return claim;
}

std::optional<Answer> McppProvider::existing(const Claim&, const ProviderContext&) const {
    return std::nullopt;
}

Answer McppProvider::describe(const Claim& claim, const ProviderContext& context) const {
    const Detection detection { .kind = SourceKind::mcpp, .root = claim.root, .manifest = claim.manifest,
                                .buildDirectory = claim.buildDirectory, .compileCommands = claim.compileCommands,
                                .buildDatabase = claim.buildDatabase };
    return from_result(load_mcpp(detection, context));
}

std::vector<std::string> McppProvider::watch_inputs(const Claim&) const {
    return { "mcpp.toml", "mcpp.lock", ".xlings.json", "compile_commands.json", std::string { MCPP_LAST_RESULT } };
}

} // namespace mcppls::project
