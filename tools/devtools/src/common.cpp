module mcppls.devtools.common;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.toolrun;

namespace mcppls::devtools {
namespace {

namespace fs = mcppls::platform::fs;
namespace env = mcppls::platform::env;

bool is_repository(const std::string& directory) {
    const std::string manifest { base::join_path(directory, "mcpp.toml") };
    if (!fs::is_regular_file(manifest) || !fs::is_directory(base::join_path(directory, "packaging"))) return false;
    const auto text = fs::read_file(manifest);
    return text && text->contains("[workspace]");
}

std::optional<std::string> upward(std::string directory) {
    for (int level { 0 }; level < 12 && !directory.empty(); ++level) {
        if (is_repository(directory)) return directory;
        const std::string parent { base::parent_path(directory) };
        if (parent == directory) break;
        directory = parent;
    }
    return std::nullopt;
}

// Windows cannot start a batch file directly: the command interpreter runs it.
std::pair<std::string, std::vector<std::string>> runnable(const std::string& program, std::vector<std::string> arguments) {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        const std::string lowered { base::to_lower_ascii(program) };
        if (lowered.ends_with(".cmd") || lowered.ends_with(".bat")) {
            std::vector<std::string> wrapped { "/d", "/c", program };
            std::ranges::move(arguments, std::back_inserter(wrapped));
            return { base::normalize_path(env::get("ComSpec").value_or("C:/Windows/System32/cmd.exe")), std::move(wrapped) };
        }
    }
    return { program, std::move(arguments) };
}

} // namespace

std::string repository_root() {
    if (auto found = upward(base::normalize_path(fs::current_directory()))) return *found;
    const auto arguments = env::arguments();
    std::string self { arguments.empty() ? std::string {} : arguments.front() };
    if (!self.empty()) {
        if (!base::is_absolute_path(self)) self = base::join_path(fs::current_directory(), self);
        if (auto found = upward(base::parent_path(base::normalize_path(self)))) return *found;
    }
    return fs::current_directory();
}

bool step(std::string_view what, const std::string& program, std::vector<std::string> arguments,
          const std::string& directory) {
    std::println("  {} ...", what);
    auto [toRun, toPass] = runnable(program, std::move(arguments));
    base::log::debug("{} {}", toRun, base::join(toPass, " "));
    const auto started = std::chrono::steady_clock::now();
    auto result = platform::toolrun::run({
        .program = toRun,
        .arguments = std::move(toPass),
        .workDirectory = directory,
        .purpose = "devtools",
        .network = platform::toolrun::Network::allowed,   // packaging fetches clangd and npm packages
        .bounds = platform::RunBounds { .hard = std::chrono::hours { 2 } },
    });
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
    if (!result) {
        std::println(std::cerr, "  {} could not start: {}", program, result.error().message);
        return false;
    }
    if (result->exitCode != 0 || result->timedOut) {
        std::println(std::cerr, "  {} failed ({}) after {} s", what, result->exitCode, seconds);
        const std::string tail { platform::last_lines(result->error.empty() ? result->output : result->error, 30) };
        if (!tail.empty()) std::println(std::cerr, "{}", tail);
        return false;
    }
    std::println("  {} took {} s", what, seconds);
    return true;
}

base::Result<std::string> capture(const std::string& program, std::vector<std::string> arguments,
                                  const std::string& directory, std::chrono::milliseconds bound) {
    auto [toRun, toPass] = runnable(program, std::move(arguments));
    auto result = platform::toolrun::run({
        .program = toRun,
        .arguments = std::move(toPass),
        .workDirectory = directory,
        .purpose = "devtools",
        .network = platform::toolrun::Network::allowed,
        .bounds = platform::RunBounds { .hard = bound },
    });
    if (!result) return std::unexpected { result.error() };
    if (result->exitCode != 0 || result->timedOut) {
        return base::fail("devtools-run", std::format("{} exited {}{}\n{}", program, result->exitCode,
                                                      result->timedOut ? " (timed out)" : "",
                                                      platform::last_lines(result->error, 30)));
    }
    return std::move(result->output);
}

std::optional<std::string> tool(std::string_view name, std::string_view whatItIsFor) {
    if (auto found = env::find_executable(name)) return *found;
    std::println(std::cerr, "mcppls-devtools: {} is not on PATH — {}.", name, whatItIsFor);
    std::println(std::cerr, "  It is declared in mcpp.toml's [xlings.workspace]; running this "
                            "through `mcpp run -p devtools -- ...` provisions it.");
    return std::nullopt;
}

base::Result<std::string> locate_server(const std::string& root, const ServerBuild& build) {
    auto mcpp = env::find_executable("mcpp");
    if (!mcpp) return base::fail("devtools-mcpp", "mcpp is not on PATH, so the server cannot be built; pass --server");
    std::vector<std::string> arguments { "build", "--profile", build.profile, "--print-fingerprint" };
    if (!build.target.empty()) {
        arguments.push_back("--target");
        arguments.push_back(build.target);
    }
    std::println("  building the server: mcpp {}", base::join(arguments, " "));
    auto output = capture(*mcpp, arguments, root, std::chrono::hours { 1 });
    if (!output) return std::unexpected { output.error() };

    // `Fingerprint: <hex>` names the directory the build wrote, target/<triple>/<fingerprint>/.
    // Asking for it is what makes this exact: the newest mcppls under target/ is whichever
    // profile or target was built last, which is not necessarily the one asked for here.
    std::string fingerprint;
    for (const auto line : base::split_lines(*output)) {
        const std::string_view trimmed { base::trim(line) };
        if (trimmed.starts_with("Fingerprint:")) fingerprint = std::string { base::trim(trimmed.substr(12)) };
    }
    if (fingerprint.empty()) return base::fail("devtools-mcpp", "mcpp build --print-fingerprint printed no fingerprint");
    const std::string targets { base::join_path(root, "target") };
    for (const auto& triple : fs::list_directory(targets)) {
        for (const std::string_view suffix : { "", ".exe" }) {
            const std::string server { base::join_path(triple, std::format("{}/bin/mcppls{}", fingerprint, suffix)) };
            if (fs::is_regular_file(server)) return server;
        }
    }
    return base::fail("devtools-mcpp", std::format("no mcppls under {}/*/{}/bin", targets, fingerprint));
}

void print_json(const nlohmann::json& value) { std::println("{}", value.dump(2)); }

} // namespace mcppls::devtools
