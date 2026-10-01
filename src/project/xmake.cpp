module mcppls.project.xmake;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.log;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.platform.toolenv;
import mcppls.spec.discovery;
import mcppls.project.infer;
import mcppls.project.provider;

namespace mcppls::project {

namespace fs = platform::fs;

namespace {

// Adds (or replaces) `extra` on top of the tool environment (design 4.2, 4.3): a build tool the
// server starts by itself sees what the user's own build would, plus what this provider needs
// xmake to see, never a wholesale replacement of it.
std::vector<std::string> environment_with(const std::vector<std::pair<std::string, std::string>>& extra, std::chrono::milliseconds wait) {
    auto environment = platform::toolenv::get(wait).variables;
    for (const auto& [name, value] : extra) {
        std::erase_if(environment, [&](const std::string& entry) { return entry.starts_with(name + "="); });
        environment.push_back(std::format("{}={}", name, value));
    }
    return environment;
}

// A Lua string literal from `text[index]` (the quote), escapes undone; `index` ends after the closing quote.
std::string read_lua_string(std::string_view text, std::size_t& index) {
    const char quote { text[index++] };
    std::string value;
    while (index < text.size() && text[index] != quote) {
        if (text[index] == '\\' && index + 1 < text.size()) {
            ++index;
            value += text[index] == 'n' ? '\n' : text[index] == 't' ? '\t' : text[index];
        } else {
            value += text[index];
        }
        ++index;
    }
    if (index < text.size()) ++index;
    return value;
}

// Moves past one value of the table, nested tables and strings included, stopping at the `,` or `}` that ends it.
void skip_lua_value(std::string_view text, std::size_t& index) {
    int depth { 0 };
    while (index < text.size()) {
        const char c { text[index] };
        if (c == '"' || c == '\'') {
            (void)read_lua_string(text, index);
            continue;
        }
        if (c == '{') ++depth;
        if (c == '}') {
            if (depth == 0) return;
            --depth;
        }
        if (c == ',' && depth == 0) return;
        ++index;
    }
}

void skip_space(std::string_view text, std::size_t& index) {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
}

// The xmake.lua files of a project: the root's and every other under it (`includes("src")` reads `src/xmake.lua`).
// Bounded, since this runs on the fallback path only and a repository can be large.
std::vector<std::string> xmake_manifests(const Claim& claim) {
    std::vector<std::string> manifests { claim.manifest };
    static constexpr std::array<std::string_view, 7> SKIP { "build", "node_modules", "target", "third_party", "vendor", "out", "bin" };
    static constexpr std::array<std::string_view, 1> EXTENSIONS { ".lua" };
    for (const auto& file : fs::list_files(claim.root, EXTENSIONS, SKIP)) {
        if (manifests.size() >= 200) break;
        if (base::file_name(file) == "xmake.lua" && !base::same_path(file, claim.manifest)) manifests.push_back(file);
    }
    return manifests;
}

// The project's own compile_commands.json, read as it is (the fallback of X-1; what 0.0.7 always did). When it is older than
// the inputs it was generated from, the model cannot follow them: one notice, with the one action (plan 0.0.8 part 2, X-1).
Answer read_project_database(const Claim& claim, const ProviderContext& context) {
    Answer answer { from_result(database_from_compile_commands_file(claim.compileCommands, claim.root, "xmake", context.scanner, context.prober)) };
    if (!answer.database) return answer;
    // Only here is the file watched: while xmake runs, the file is not a source and its changes mean nothing.
    answer.database->watch.push_back(claim.compileCommands);
    const auto commands { fs::stamp(claim.compileCommands) };
    if (!commands) return answer;
    std::vector<std::int64_t> inputs;
    for (const auto& manifest : xmake_manifests(claim)) {
        if (const auto stamp = fs::stamp(manifest)) inputs.push_back(stamp->modified);
    }
    if (const auto user = read_xmake_user_config(claim.root)) inputs.push_back(user->modified);
    if (xmake_commands_out_of_date(commands->modified, inputs)) {
        answer.database->notices.emplace_back("xmake-commands-stale",
            std::format("{} is older than xmake.lua, so changes to it are not reflected; run `xmake project -k compile_commands` to update it, {}",
                        base::file_name(claim.compileCommands),
                        context.runBuildTool ? "or put xmake on PATH so that mcppls runs it" : "or turn on mcppls.buildTool"));
    }
    return answer;
}

// D-1 (plan 0.0.9): what `xmake f` accepts in this project, from `xmake f --help` run where configure runs (the project's
// directory, the same environment, offline). Kept in the private directory under xmake's path and time and xmake.lua's stamp, so
// it is asked once and again only when either changes. Empty when xmake gave no usable help: the caller keeps the reduced retry.
std::vector<std::string> accepted_options(const Claim& claim, const ProviderContext& context, const std::string& xmake,
                                          const std::vector<std::string>& environment) {
    const auto toolStamp { fs::stamp(xmake) };
    const auto manifestStamp { fs::stamp(claim.manifest) };
    const std::string key { std::format("{} {} {} {} {}", xmake, toolStamp ? toolStamp->modified : 0, toolStamp ? toolStamp->size : 0,
                                        manifestStamp ? manifestStamp->modified : 0, manifestStamp ? manifestStamp->size : 0) };
    const std::string cachePath { base::join_path(context.privateDirectory, "xmake-help") };
    if (const auto cached = fs::read_file(cachePath)) {
        const auto lines { base::split_lines(*cached) };
        if (!lines.empty() && lines.front() == key) {
            std::vector<std::string> names;
            for (const auto line : lines | std::views::drop(1)) {
                if (!base::trim(line).empty()) names.emplace_back(base::trim(line));
            }
            return names;
        }
    }
    const auto help = platform::toolrun::run({
        .program = xmake,
        .arguments = { "f", "--help" },
        .workDirectory = claim.root,
        .purpose = "xmake-help",
        .root = context.rootKey,
        .network = platform::toolrun::Network::offline,
        .bounds = platform::RunBounds { .hard = std::chrono::seconds { 15 } },
        .environment = environment,
        .environmentWait = context.environmentWait,
    });
    if (!help || help->timedOut) return {};
    const auto names { xmake_help_options(help->output + "\n" + help->error) };
    // A help text with no `--plat` is not xmake's help of `f`: nothing is learned from it, and nothing is kept.
    if (!std::ranges::contains(names, std::string { "plat" })) return {};
    (void)fs::write_file(cachePath, key + "\n" + base::join(names, "\n"));
    return names;
}

// D-2, D-6: the failure of either stage as the user can act on it, when the output names packages xmake could not have: offline,
// the download that the user decides; online, an install that was tried and failed. Empty when the output names no package.
std::optional<Answer> missing_packages_answer(const std::string& output, bool offline) {
    auto missing { xmake_missing_packages(output) };
    if (!offline && missing.empty()) {
        // With the network allowed xmake does not say "not found": it says which install failed, and where its log is.
        missing = xmake_failed_installs(output);
        if (missing.empty() && xmake_install_log(output).empty()) return std::nullopt;
    } else if (missing.empty()) {
        return std::nullopt;
    }
    if (offline) {
        return Answer { .outcome = Outcome::needs_download, .code = std::string { spec::NEEDS_DOWNLOAD },
                        .reason = xmake_needs_download_reason(missing), .missing = std::move(missing) };
    }
    return Answer { .code = std::string { spec::INSTALL_FAILED }, .reason = xmake_install_failed_reason(missing, output), .missing = std::move(missing) };
}

// Runs xmake privately: two commands, in a directory of ours, never the project's (see the comment in the body).
Answer run_private(const Claim& claim, const ProviderContext& context, const std::string& xmake) {
    context.producerUsed = xmake;

    // Isolation (plan §3.5): `xmake project -k compile_commands` alone still writes `build/` into
    // the project (its implicit configuration and module dependency scan, both measured side
    // effects of that command) unless it is given a private `--builddir` -- which only `xmake f`
    // can set, so a first run is two commands; `XMAKE_CONFIGDIR` keeps the configuration itself out
    // of the project's own `.xmake/` too.
    const std::string configDirectory { base::join_path(context.privateDirectory, "config") };
    const std::string buildDirectory { base::join_path(context.privateDirectory, "build") };
    const std::string outDirectory { base::join_path(context.privateDirectory, "out") };
    (void)fs::create_directories(configDirectory);
    (void)fs::create_directories(outDirectory);
    const auto environment { environment_with(xmake_environment(configDirectory), context.environmentWait) };

    // Keep it simple (B-1 task 3): configure again whenever there is no private configuration yet,
    // or it was made for another mode, another xmake.lua or another `xmake f` of the user's (the key recorded after the last success).
    const std::string configuredMarker { base::join_path(context.privateDirectory, "configured") };
    const std::string leftOutMarker { base::join_path(context.privateDirectory, "left-out") };
    const auto manifestStamp { fs::stamp(claim.manifest) };
    // X-2: the user's own `xmake f` is read, never run or written; `xmake f` resets every option it is not given (E5).
    const auto user { read_xmake_user_config(claim.root) };
    const std::string key { xmake_configuration_key(
        context.offline, manifestStamp ? std::optional { std::pair { manifestStamp->size, manifestStamp->modified } } : std::nullopt,
        user ? std::optional { std::pair { user->size, user->modified } } : std::nullopt) };
    const bool hasConfig { fs::is_directory(configDirectory) && !fs::list_directory(configDirectory).empty() };
    const auto recorded { fs::read_file(configuredMarker) };
    const bool stale { !recorded || *recorded != key };
    if (!hasConfig || stale) {
        fs::remove_all(configuredMarker);   // a failed `xmake f -c` leaves no configuration worth keeping
        fs::remove_all(leftOutMarker);
        // D-1: asked only when xmake.conf holds something that is neither internal nor standard; the answer is kept.
        std::vector<std::string> accepted;
        if (user && !xmake_left_out(user->options).empty()) {
            accepted = accepted_options(claim, context, xmake, environment);
            if (const auto skipped { xmake_not_accepted(user->options, accepted) }; !skipped.empty()) {
                base::log::info("xmake f does not take {} of {}; these are keys xmake wrote itself, so they are not passed on", base::join(skipped, ", "), user->path);
            }
        }
        const auto configure = [&](bool reduced) {
            return platform::toolrun::run({
                .program = xmake,
                .arguments = xmake_configure_arguments(buildDirectory, context.offline, user ? std::span<const XmakeOption> { user->options } : std::span<const XmakeOption> {}, reduced, accepted),
                .workDirectory = claim.root,
                .purpose = "configure",
                .root = context.rootKey,
                .network = context.offline ? platform::toolrun::Network::offline : platform::toolrun::Network::allowed,
                .bounds = platform::RunBounds { .hard = context.configureTimeout },
                .soft = context.producerSoft,
                .environment = environment,
                .environmentWait = context.environmentWait,
                .onSoftDeadline = context.onSlow,
            });
        };
        auto configured = configure(false);
        // X-2: an option the user's xmake.conf still holds, that xmake.lua no longer declares, is refused (exit 255 with v3.1.1).
        // One retry with the standard options only; what was left out is said in a notice. D-1: with the list `xmake f --help`
        // gave there is nothing left to refuse, so this is the fallback for when that list could not be had.
        if (configured && !configured->timedOut && configured->exitCode != 0 && user && !xmake_left_out(user->options).empty()
            && xmake_unknown_option(configured->output + "\n" + configured->error)) {
            const auto leftOut { xmake_left_out(user->options) };
            base::log::info("xmake did not accept the options of {} ({}); configuring again with the standard ones only", user->path, base::join(leftOut, ", "));
            configured = configure(true);
            if (configured && configured->exitCode == 0 && !configured->timedOut) (void)fs::write_file(leftOutMarker, base::join(leftOut, ", "));
        }
        if (!configured) return Answer { .code = configured.error().code, .reason = configured.error().message };
        if (configured->timedOut) {
            return Answer { .outcome = Outcome::timed_out, .code = "xmake-configure-failed", .reason = "xmake f did not finish in time" };
        }
        if (configured->exitCode != 0) {
            const std::string combined { configured->output + "\n" + configured->error };
            if (auto packages = missing_packages_answer(combined, context.offline)) return std::move(*packages);
            return Answer { .code = "xmake-configure-failed",
                            .reason = std::format("xmake f failed ({}): {}", configured->exitCode,
                                                  base::trim(configured->error.empty() ? configured->output : configured->error)) };
        }
        (void)fs::write_file(configuredMarker, key);
    }

    auto described = platform::toolrun::run({
        .program = xmake,
        .arguments = xmake_compile_commands_arguments(outDirectory),
        .workDirectory = claim.root,
        .purpose = "producer",
        .root = context.rootKey,
        .network = platform::toolrun::Network::offline,
        .bounds = platform::RunBounds { .hard = context.producerHard },
        .soft = context.producerSoft,
        .environment = environment,
        .environmentWait = context.environmentWait,
        .onSoftDeadline = context.onSlow,
    });
    if (!described) return Answer { .code = described.error().code, .reason = described.error().message };
    if (described->timedOut) {
        return Answer { .outcome = Outcome::timed_out, .code = "xmake-configure-failed",
                        .reason = "xmake project -k compile_commands did not finish in time" };
    }
    if (described->exitCode != 0) {
        // D-6: the project stage reads the packages too; they can be missing here when `xmake f` did not need them.
        if (auto packages = missing_packages_answer(described->output + "\n" + described->error, context.offline)) return std::move(*packages);
        return Answer { .code = "xmake-configure-failed",
                        .reason = std::format("xmake project -k compile_commands failed ({}): {}", described->exitCode,
                                              base::trim(described->error.empty() ? described->output : described->error)) };
    }
    const std::string commands { base::join_path(outDirectory, "compile_commands.json") };
    if (!fs::is_regular_file(commands)) return Answer { .code = "xmake-no-database", .reason = "xmake did not write compile_commands.json" };
    Answer answer { from_result(database_from_compile_commands_file(commands, claim.root, "xmake", context.scanner, context.prober)) };
    if (answer.database) {
        if (const auto leftOut = fs::read_file(leftOutMarker); leftOut && !leftOut->empty()) {
            answer.database->notices.emplace_back("xmake-options-left-out",
                std::format("xmake refused one of the options in your .xmake xmake.conf, so mcppls configured without {} (platform, mode and toolchain are kept) and "
                            "the commands may differ from your build's; run `xmake f -c` in the project to renew its configuration", *leftOut));
        }
    }
    return answer;
}

} // namespace

std::vector<XmakeOption> parse_xmake_conf(std::string_view text) {
    std::vector<XmakeOption> options;
    std::size_t index { text.find('{') };
    if (index == std::string_view::npos) return options;
    ++index;
    while (index < text.size()) {
        const char c { text[index] };
        if (c == '}') break;
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            ++index;
            continue;
        }
        if (!base::is_identifier_start(c)) {   // `"envs",` or `["k"] = v`: not a key this reads
            skip_lua_value(text, index);
            continue;
        }
        const std::size_t start { index };
        while (index < text.size() && base::is_identifier_char(text[index])) ++index;
        const std::string name { text.substr(start, index - start) };
        skip_space(text, index);
        if (index >= text.size() || text[index] != '=' || (index + 1 < text.size() && text[index + 1] == '=')) {
            skip_lua_value(text, index);
            continue;
        }
        ++index;
        skip_space(text, index);
        if (index >= text.size()) break;
        if (text[index] == '"' || text[index] == '\'') {
            options.push_back(XmakeOption { name, read_lua_string(text, index), false });
            continue;
        }
        const std::size_t wordStart { index };
        while (index < text.size() && base::is_identifier_char(text[index])) ++index;
        const std::string_view word { text.substr(wordStart, index - wordStart) };
        // `true`/`false` only as the whole value: a table or an expression is not a scalar to hand to `xmake f`.
        std::size_t after { index };
        skip_space(text, after);
        const bool ends { after >= text.size() || text[after] == ',' || text[after] == '}' };
        if ((word == "true" || word == "false") && ends) {
            options.push_back(XmakeOption { name, std::string { word }, true });
        } else {
            index = wordStart;
            skip_lua_value(text, index);
        }
    }
    return options;
}

std::optional<XmakeUserConfig> read_xmake_user_config(std::string_view root) {
    const std::string dot { base::join_path(std::string { root }, ".xmake") };
    std::optional<XmakeUserConfig> best;
    for (const auto& platform : fs::list_directory(dot)) {
        if (!fs::is_directory(platform)) continue;
        for (const auto& architecture : fs::list_directory(platform)) {
            const std::string path { base::join_path(architecture, "xmake.conf") };
            const auto stamp { fs::stamp(path) };
            if (!stamp || !fs::is_regular_file(path)) continue;
            if (best && best->modified >= stamp->modified) continue;   // the newest is the user's last `xmake f`
            best = XmakeUserConfig { path, stamp->size, stamp->modified, {} };
        }
    }
    if (!best) return std::nullopt;
    if (const auto text = fs::read_file(best->path)) best->options = parse_xmake_conf(*text);
    return best;
}

bool xmake_option_is_internal(std::string_view name) {
    static constexpr std::array<std::string_view, 10> INTERNAL { "builddir", "host", "theme", "network", "proxy_pac", "ccache",
                                                                 "policies", "pkg_searchdirs", "ndk_stdcxx", "clean" };
    return name.starts_with("__") || std::ranges::find(INTERNAL, name) != INTERNAL.end();
}

bool xmake_option_is_standard(std::string_view name) {
    static constexpr std::array<std::string_view, 7> STANDARD { "plat", "arch", "mode", "toolchain", "sdk", "runtimes", "kind" };
    return std::ranges::find(STANDARD, name) != STANDARD.end();
}

std::vector<std::string> xmake_left_out(std::span<const XmakeOption> user) {
    std::vector<std::string> names;
    for (const auto& option : user) {
        if (!xmake_option_is_internal(option.name) && !xmake_option_is_standard(option.name)) names.push_back(option.name);
    }
    return names;
}

std::vector<std::string> xmake_help_options(std::string_view text) {
    std::vector<std::string> names;
    for (const auto line : base::split_lines(text)) {
        std::size_t indent { 0 };
        while (indent < line.size() && line[indent] == ' ') ++indent;
        if (indent == 0 || indent > 8 || indent >= line.size() || line[indent] != '-') continue;
        // `-p PLAT, --plat=PLAT   Compile for ...`: the spellings come before the first run of two spaces.
        std::string_view spellings { line.substr(indent) };
        if (const std::size_t gap { spellings.find("  ") }; gap != std::string_view::npos) spellings = spellings.substr(0, gap);
        for (const auto part : base::split(spellings, ',')) {
            std::string_view spelling { base::trim(part) };
            if (!spelling.starts_with("--")) continue;
            spelling.remove_prefix(2);
            std::size_t end { 0 };
            while (end < spelling.size() && (base::is_identifier_char(spelling[end]) || spelling[end] == '-')) ++end;
            if (end > 0) names.emplace_back(spelling.substr(0, end));
        }
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());
    return names;
}

std::vector<std::string> xmake_not_accepted(std::span<const XmakeOption> user, std::span<const std::string> accepted) {
    std::vector<std::string> names;
    if (accepted.empty()) return names;
    for (const auto& option : user) {
        if (xmake_option_is_internal(option.name) || xmake_option_is_standard(option.name)) continue;
        if (!std::ranges::contains(accepted, option.name)) names.push_back(option.name);
    }
    return names;
}

std::vector<std::string> xmake_error_lines(std::string_view output, std::size_t limit) {
    std::vector<std::string> lines;
    for (const auto line : base::split_lines(output)) {
        const std::string_view trimmed { base::trim(line) };
        if (base::to_lower_ascii(trimmed).starts_with("error:")) lines.emplace_back(trimmed);
    }
    if (lines.size() > limit) lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(limit));
    return lines;
}

std::vector<std::string> xmake_failed_installs(std::string_view output) {
    std::vector<std::string> names;
    for (const auto line : base::split_lines(output)) {
        const std::string lower { base::to_lower_ascii(base::trim(line)) };
        if (!lower.starts_with("error: install ") || !lower.contains(" failed")) continue;
        const std::string_view rest { std::string_view { lower }.substr(std::string_view { "error: install " }.size()) };
        if (const std::string name { rest.substr(0, rest.find(' ')) }; !name.empty() && !std::ranges::contains(names, name)) names.push_back(name);
    }
    return names;
}

std::string xmake_install_log(std::string_view output) {
    for (const auto line : base::split_lines(output)) {
        for (const auto word : base::split(line, ' ')) {
            if (!word.contains("installdir.failed")) continue;
            std::string_view path { base::trim(word) };
            while (!path.empty() && (path.back() == '.' || path.back() == ',' || path.back() == ')' || path.back() == '\'' || path.back() == '"')) path.remove_suffix(1);
            while (!path.empty() && (path.front() == '(' || path.front() == '\'' || path.front() == '"')) path.remove_prefix(1);
            if (!path.empty()) return std::string { path };
        }
    }
    return {};
}

std::string xmake_needs_download_reason(std::span<const std::string> missing) {
    return std::format("xmake needs {} downloaded: they are what xmake would fetch or build for this project's requirements (build tools "
                       "included), and this run stayed offline (network.mode:private); installing them with your system package manager "
                       "also works, because xmake uses the system's when it finds them",
                       base::join(missing, ", "));
}

std::string xmake_install_failed_reason(std::span<const std::string> missing, std::string_view output) {
    std::string reason { std::format("xmake could not install {} (the network was allowed)",
                                     missing.empty() ? std::string { "the packages this project requires" } : base::join(missing, ", ")) };
    for (const auto& line : xmake_error_lines(output)) reason += std::format("; {}", line);
    if (const std::string log { xmake_install_log(output) }; !log.empty()) reason += std::format("; its log is {}", log);
    reason += "; installing them with your system package manager also works, because xmake uses the system's when it finds them";
    return reason;
}

bool xmake_unknown_option(std::string_view output) {
    const std::string lower { base::to_lower_ascii(output) };
    return lower.contains("invalid option") || lower.contains("unknown option");
}

bool xmake_commands_out_of_date(std::int64_t commandsModified, std::span<const std::int64_t> inputsModified) {
    return std::ranges::any_of(inputsModified, [&](std::int64_t modified) { return modified > commandsModified; });
}

std::vector<std::pair<std::string, std::string>> xmake_environment(std::string_view configDirectory) {
    return { { "XMAKE_CONFIGDIR", std::string { configDirectory } }, { "XMAKE_THEME", "plain" } };
}

std::string xmake_configuration_key(bool offline, std::optional<std::pair<std::uint64_t, std::int64_t>> manifest,
                                    std::optional<std::pair<std::uint64_t, std::int64_t>> userConfig) {
    std::string key { std::format("{} {} {}", offline ? "offline" : "online", manifest ? manifest->first : 0, manifest ? manifest->second : 0) };
    // X-2: no `.xmake/xmake.conf` keeps the key 0.0.7 had, so a project without one is not configured again by the upgrade.
    if (userConfig) key += std::format(" conf {} {}", userConfig->first, userConfig->second);
    return key;
}

std::vector<std::string> xmake_configure_arguments(std::string_view buildDirectory, bool offline,
                                                   std::span<const XmakeOption> user, bool reduced, std::span<const std::string> accepted) {
    std::vector<std::string> arguments { "f", "-c" };
    // X-2: what the user chose with their own `xmake f`. An option that is internal, unnamed in a form `xmake f` takes, or
    // empty is not passed on; a retry (`reduced`) passes the standard ones only.
    const auto wanted = [&](const XmakeOption& option) {
        return !option.name.empty() && !xmake_option_is_internal(option.name) && !(reduced && !xmake_option_is_standard(option.name))
            && !option.value.empty() && std::ranges::all_of(option.name, [](char c) { return base::is_identifier_char(c) || c == '-'; })
            && (accepted.empty() || xmake_option_is_standard(option.name) || std::ranges::contains(accepted, option.name));
    };
    const auto value_of = [](const XmakeOption& option) { return option.flag ? std::string { option.value == "true" ? "y" : "n" } : option.value; };
    // plat, arch and mode have short options, given in that order however the file lists them.
    for (const auto& [name, flag] : std::array<std::pair<std::string_view, std::string_view>, 3> { { { "plat", "-p" }, { "arch", "-a" }, { "mode", "-m" } } }) {
        const auto found = std::ranges::find_if(user, [&](const XmakeOption& option) { return option.name == name && wanted(option); });
        if (found == user.end()) continue;
        arguments.emplace_back(flag);
        arguments.push_back(value_of(*found));
    }
    for (const auto& option : user) {
        if (!wanted(option) || option.name == "plat" || option.name == "arch" || option.name == "mode") continue;
        arguments.push_back(std::format("--{}={}", option.name, value_of(option)));
    }
    if (offline) {
        // Measured (plan §3.5): `package.fetch_only` alone still lets xmake update its package
        // repositories on a machine that has never fetched them ("updating repositories", 11 s,
        // writing into `~/.xmake`); `network.mode:private` is what its own `pulled()` check reads to
        // skip that, and together the two stop this run reaching the network at all.
        arguments.emplace_back("--confirm=no");
        arguments.emplace_back("--policies=package.fetch_only,network.mode:private");
    } else {
        arguments.emplace_back("-y");
    }
    arguments.push_back(std::format("--builddir={}", buildDirectory));
    return arguments;
}

std::vector<std::string> xmake_compile_commands_arguments(std::string_view outDirectory) {
    return { "project", "-k", "compile_commands", std::string { outDirectory } };
}

std::vector<std::string> xmake_missing_packages(std::string_view output) {
    std::vector<std::string> names;
    if (const std::size_t marker { output.find("packages(") }; marker != std::string_view::npos) {
        const std::size_t open { marker + std::string_view { "packages(" }.size() - 1 };
        if (const std::size_t close { output.find(')', open) }; close != std::string_view::npos) {
            for (const auto part : base::split(output.substr(open + 1, close - open - 1), ',')) {
                if (const auto name = base::trim(part); !name.empty()) names.emplace_back(name);
            }
        }
    }
    if (!names.empty()) return names;
    // The other shape: "note: install or modify (m) these packages" followed by one "  -> name
    // version: ..." line per package (xmake's interactive prompt format, still printed when nothing
    // is there to answer it).
    if (output.contains("install or modify (m) these packages")) {
        for (const auto line : base::split_lines(output)) {
            std::string_view trimmed { base::trim(line) };
            if (!trimmed.starts_with("->")) continue;
            trimmed = base::trim(trimmed.substr(2));
            const std::size_t end { trimmed.find(' ') };
            if (std::string name { trimmed.substr(0, end) }; !name.empty()) names.push_back(std::move(name));
        }
    }
    return names;
}

std::optional<Claim> XmakeProvider::detect(std::string_view root) const {
    const std::string manifest { base::join_path(std::string { root }, "xmake.lua") };
    if (!fs::is_regular_file(manifest)) return std::nullopt;
    Claim claim;
    claim.provider = "xmake";
    claim.confidence = 100;
    claim.root = std::string { root };
    claim.manifest = manifest;
    // xmake's own VS Code plugin writes its compile_commands.json into .vscode/ rather than the
    // root; either is read as-is when xmake cannot be run (X-1).
    for (std::string_view candidate : { "compile_commands.json", ".vscode/compile_commands.json" }) {
        if (const std::string commands { base::join_path(std::string { root }, candidate) }; fs::is_regular_file(commands)) {
            claim.compileCommands = commands;
            break;
        }
    }
    return claim;
}

std::optional<Answer> XmakeProvider::existing(const Claim& claim, const ProviderContext& context) const {
    if (claim.compileCommands.empty()) return std::nullopt;
    // X-1 (plan 0.0.8 part 2, Q1): when xmake can be run, its private run is the only source. A file the user generated
    // follows their last run of it, not their xmake.lua (E1: a late `set_languages` never reached the model), and two sources
    // taking turns restart clangd. The file is read only when xmake cannot run: untrusted, `mcppls.buildTool: off`, not on PATH.
    if (context.trusted && context.runBuildTool && find_tool("xmake", std::vector<std::string> {})) return std::nullopt;
    return read_project_database(claim, context);
}

Answer XmakeProvider::describe(const Claim& claim, const ProviderContext& context) const {
    if (!context.trusted) return Answer { .code = "untrusted-workspace", .reason = "the workspace is not trusted, so xmake was not run" };
    if (!context.runBuildTool) return Answer { .code = "xmake-no-database", .reason = "mcppls.buildTool is off, so xmake was not run" };
    const auto xmake = find_tool("xmake", std::vector<std::string> {});
    if (!xmake) return Answer { .code = "xmake-not-found", .reason = "xmake.lua is there but xmake is not on PATH" };

    // A private run that succeeded once has produced a model from xmake; a later failure (a half-typed xmake.lua) leaves that
    // model in place rather than replacing it with an older file's.
    const bool ranBefore { fs::is_regular_file(base::join_path(base::join_path(context.privateDirectory, "out"), "compile_commands.json")) };
    Answer answer { run_private(claim, context, *xmake) };
    if (answer.database || claim.compileCommands.empty() || ranBefore) return answer;

    // X-1: xmake cannot describe this project (yet), and the user's own file can. Read, as in 0.0.7.
    base::log::info("xmake could not describe {} ({}); reading {} instead", claim.root, answer.reason, claim.compileCommands);
    Answer fallback { read_project_database(claim, context) };
    if (!fallback.database) return answer;
    context.producerUsed.clear();   // the model is the file's, not xmake's
    if (answer.outcome == Outcome::needs_download || answer.code == spec::INSTALL_FAILED) fallback.database->issues.emplace_back(answer.code, answer.reason);
    return fallback;
}

std::vector<std::string> XmakeProvider::watch_inputs(const Claim&) const {
    // The user's last `xmake f` (X-2) is an input as much as xmake.lua is; their compile_commands.json is one only while it is
    // the source (read_project_database adds it then).
    return { "xmake.lua", "**/xmake.lua", ".xmake/*/*/xmake.conf" };
}

} // namespace mcppls::project
