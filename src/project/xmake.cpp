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

} // namespace

std::vector<std::pair<std::string, std::string>> xmake_environment(std::string_view configDirectory) {
    return { { "XMAKE_CONFIGDIR", std::string { configDirectory } }, { "XMAKE_THEME", "plain" } };
}

std::string xmake_configuration_key(bool offline, std::optional<std::pair<std::uint64_t, std::int64_t>> manifest) {
    return std::format("{} {} {}", offline ? "offline" : "online", manifest ? manifest->first : 0, manifest ? manifest->second : 0);
}

std::vector<std::string> xmake_configure_arguments(std::string_view buildDirectory, bool offline) {
    std::vector<std::string> arguments { "f", "-c" };
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
    // root; either is read as-is (staleness is the model's own watch, not this provider's business).
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
    return from_result(database_from_compile_commands_file(claim.compileCommands, claim.root, "xmake", context.scanner, context.prober));
}

Answer XmakeProvider::describe(const Claim& claim, const ProviderContext& context) const {
    if (!context.trusted) return Answer { .code = "untrusted-workspace", .reason = "the workspace is not trusted, so xmake was not run" };
    if (!context.runBuildTool) return Answer { .code = "xmake-no-database", .reason = "mcppls.buildTool is off, so xmake was not run" };
    const auto xmake = find_tool("xmake", std::vector<std::string> {});
    if (!xmake) return Answer { .code = "xmake-not-found", .reason = "xmake.lua is there but xmake is not on PATH" };
    context.producerUsed = *xmake;

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
    // or it was made for another mode or another xmake.lua (the key recorded after the last success).
    const std::string configuredMarker { base::join_path(context.privateDirectory, "configured") };
    const auto manifestStamp { fs::stamp(claim.manifest) };
    const std::string key { xmake_configuration_key(
        context.offline, manifestStamp ? std::optional { std::pair { manifestStamp->size, manifestStamp->modified } } : std::nullopt) };
    const bool hasConfig { fs::is_directory(configDirectory) && !fs::list_directory(configDirectory).empty() };
    const auto recorded { fs::read_file(configuredMarker) };
    const bool stale { !recorded || *recorded != key };
    if (!hasConfig || stale) {
        fs::remove_all(configuredMarker);   // a failed `xmake f -c` leaves no configuration worth keeping
        auto configured = platform::toolrun::run({
            .program = *xmake,
            .arguments = xmake_configure_arguments(buildDirectory, context.offline),
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
        if (!configured) return Answer { .code = configured.error().code, .reason = configured.error().message };
        if (configured->timedOut) {
            return Answer { .outcome = Outcome::timed_out, .code = "xmake-configure-failed", .reason = "xmake f did not finish in time" };
        }
        if (configured->exitCode != 0) {
            const std::string combined { configured->output + "\n" + configured->error };
            if (auto missing = xmake_missing_packages(combined); !missing.empty()) {
                return Answer { .outcome = Outcome::needs_download, .code = std::string { spec::NEEDS_DOWNLOAD },
                                .reason = std::format("xmake needs {} downloaded, and this run stayed offline (network.mode:private)",
                                                      base::join(missing, ", ")),
                                .missing = missing };
            }
            return Answer { .code = "xmake-configure-failed",
                            .reason = std::format("xmake f failed ({}): {}", configured->exitCode,
                                                  base::trim(configured->error.empty() ? configured->output : configured->error)) };
        }
        (void)fs::write_file(configuredMarker, key);
    }

    auto described = platform::toolrun::run({
        .program = *xmake,
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
        return Answer { .code = "xmake-configure-failed",
                        .reason = std::format("xmake project -k compile_commands failed ({}): {}", described->exitCode,
                                              base::trim(described->error.empty() ? described->output : described->error)) };
    }
    const std::string commands { base::join_path(outDirectory, "compile_commands.json") };
    if (!fs::is_regular_file(commands)) return Answer { .code = "xmake-no-database", .reason = "xmake did not write compile_commands.json" };
    return from_result(database_from_compile_commands_file(commands, claim.root, "xmake", context.scanner, context.prober));
}

std::vector<std::string> XmakeProvider::watch_inputs(const Claim& claim) const {
    std::vector<std::string> watch { "xmake.lua", "**/xmake.lua" };
    if (!claim.compileCommands.empty()) watch.push_back(claim.compileCommands);
    return watch;
}

} // namespace mcppls::project
