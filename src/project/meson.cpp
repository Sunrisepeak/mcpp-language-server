module mcppls.project.meson;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.spec.discovery;
import mcppls.project.infer;
import mcppls.project.provider;

namespace mcppls::project {

namespace fs = platform::fs;

namespace {

// A build directory meson has already configured: `meson-private/` is meson's own marker (the
// directory ninja/meson tooling reads back), `compile_commands.json` is what this provider wants.
std::optional<std::string> find_existing_build_directory(std::string_view root) {
    for (std::string_view name : { "builddir", "build" }) {
        if (const std::string directory { base::join_path(std::string { root }, name) };
            fs::is_directory(base::join_path(directory, "meson-private")) && fs::is_regular_file(base::join_path(directory, "compile_commands.json"))) {
            return directory;
        }
    }
    for (const auto& child : fs::list_directory(root)) {
        if (fs::is_directory(base::join_path(child, "meson-private")) && fs::is_regular_file(base::join_path(child, "compile_commands.json"))) {
            return child;
        }
    }
    return std::nullopt;
}

} // namespace

std::vector<std::string> meson_setup_arguments(std::string_view privateDirectory, bool offline, bool reconfigure) {
    std::vector<std::string> arguments { "setup" };
    // meson refuses a plain `setup` into a directory that already has one (plan §3.3 task 4);
    // `--reconfigure` is what a second, later describe() into the same private directory needs.
    if (reconfigure) arguments.emplace_back("--reconfigure");
    if (offline) arguments.emplace_back("--wrap-mode=nodownload");
    arguments.push_back(std::string { privateDirectory });
    return arguments;
}

std::vector<std::string> meson_missing_subprojects(std::string_view output) {
    std::vector<std::string> names;
    const bool disabled { output.contains("wrap-based subproject downloading is disabled") || output.contains("nodownload") };
    if (!disabled) return names;
    // Best-effort: meson's own message names the subproject in quotes somewhere nearby ("Subproject
    // 'fmt' ..."); when it does not, the caller still knows a download is needed, just not of what.
    static constexpr std::string_view MARKER { "Subproject " };
    std::size_t pos { 0 };
    while (true) {
        const std::size_t found { output.find(MARKER, pos) };
        if (found == std::string_view::npos) break;
        const std::string_view rest { output.substr(found + MARKER.size()) };
        if (!rest.empty() && (rest.front() == '\'' || rest.front() == '"')) {
            const char quote { rest.front() };
            if (const std::size_t close { rest.find(quote, 1) }; close != std::string_view::npos) {
                if (std::string name { rest.substr(1, close - 1) }; !name.empty() && std::ranges::find(names, name) == names.end()) {
                    names.push_back(std::move(name));
                }
            }
        }
        pos = found + MARKER.size();
    }
    return names;
}

std::optional<Claim> MesonProvider::detect(std::string_view root) const {
    const std::string manifest { base::join_path(std::string { root }, "meson.build") };
    if (!fs::is_regular_file(manifest)) return std::nullopt;
    Claim claim;
    claim.provider = "meson";
    claim.confidence = 100;
    claim.root = std::string { root };
    claim.manifest = manifest;
    if (const auto directory = find_existing_build_directory(root)) {
        claim.buildDirectory = *directory;
        claim.compileCommands = base::join_path(*directory, "compile_commands.json");
    }
    return claim;
}

std::optional<Answer> MesonProvider::existing(const Claim& claim, const ProviderContext& context) const {
    if (claim.compileCommands.empty()) return std::nullopt;
    return from_result(database_from_compile_commands_file(claim.compileCommands, claim.root, "meson", context.scanner, context.prober));
}

Answer MesonProvider::describe(const Claim& claim, const ProviderContext& context) const {
    if (!context.trusted) return Answer { .code = "untrusted-workspace", .reason = "the workspace is not trusted, so meson was not run" };
    if (!context.runBuildTool) return Answer { .code = "meson-no-database", .reason = "mcppls.buildTool is off, so meson was not run" };
    const auto meson = find_tool("meson", std::vector<std::string> {});
    if (!meson) return Answer { .code = "meson-not-found", .reason = "meson.build is there but meson is not on PATH" };
    context.producerUsed = *meson;
    const std::string privateDirectory { context.privateDirectory };
    (void)fs::create_directories(privateDirectory);
    const bool reconfigure { fs::is_directory(base::join_path(privateDirectory, "meson-private")) };

    auto result = platform::toolrun::run({
        .program = *meson,
        .arguments = meson_setup_arguments(privateDirectory, context.offline, reconfigure),
        .workDirectory = claim.root,
        .purpose = reconfigure ? "configure" : "configure-first",
        .root = context.rootKey,
        .network = context.offline ? platform::toolrun::Network::offline : platform::toolrun::Network::allowed,
        .bounds = platform::RunBounds { .hard = context.configureTimeout },
        .soft = context.producerSoft,
        .environmentWait = context.environmentWait,
        .onSoftDeadline = context.onSlow,
    });
    if (!result) return Answer { .code = result.error().code, .reason = result.error().message };
    if (result->timedOut) return Answer { .outcome = Outcome::timed_out, .code = "meson-configure-failed", .reason = "meson setup did not finish in time" };
    if (result->exitCode != 0) {
        const std::string combined { result->output + "\n" + result->error };
        if (context.offline) {
            if (auto missing = meson_missing_subprojects(combined); !missing.empty() || combined.contains("nodownload")) {
                return Answer { .outcome = Outcome::needs_download, .code = std::string { spec::NEEDS_DOWNLOAD },
                                .reason = missing.empty()
                                              ? std::string { "meson needs a wrap-based subproject downloaded, and this run stayed offline (--wrap-mode=nodownload)" }
                                              : std::format("meson needs {} downloaded, and this run stayed offline (--wrap-mode=nodownload)",
                                                            base::join(missing, ", ")),
                                .missing = missing };
            }
        }
        return Answer { .code = "meson-configure-failed",
                        .reason = std::format("meson setup failed ({}): {}", result->exitCode,
                                              base::trim(result->error.empty() ? result->output : result->error)) };
    }
    const std::string commands { base::join_path(privateDirectory, "compile_commands.json") };
    if (!fs::is_regular_file(commands)) return Answer { .code = "meson-no-database", .reason = "meson did not write compile_commands.json" };
    return from_result(database_from_compile_commands_file(commands, claim.root, "meson", context.scanner, context.prober));
}

std::vector<std::string> MesonProvider::watch_inputs(const Claim& claim) const {
    std::vector<std::string> watch { "meson.build", "**/meson.build", "meson_options.txt", "meson.options" };
    if (!claim.compileCommands.empty()) watch.push_back(claim.compileCommands);
    return watch;
}

} // namespace mcppls::project
