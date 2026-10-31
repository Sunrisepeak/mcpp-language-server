module mcppls.devtools.common;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcpplibs.os;
import mcppls.toolrun;
import mcppls.pack.archive;

namespace os = mcpplibs::os;
namespace mcppls::devtools {
namespace pack = mcppls::pack;
namespace {
bool is_repository(const std::string& directory) {
    std::error_code ignored;
    const std::string manifest { base::join_path(directory, "mcpp.toml") };
    if (!std::filesystem::is_regular_file(manifest, ignored) || !std::filesystem::is_directory(base::join_path(directory, "packaging"), ignored)) return false;
    const auto text = os::fs::read(manifest);
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
    if (os::host() == os::platform::windows) {
        const std::string lowered { base::to_lower_ascii(program) };
        if (lowered.ends_with(".cmd") || lowered.ends_with(".bat")) {
            std::vector<std::string> wrapped { "/d", "/c", program };
            std::ranges::move(arguments, std::back_inserter(wrapped));
            return { base::normalize_path(os::env::get("ComSpec").value_or("C:/Windows/System32/cmd.exe")), std::move(wrapped) };
        }
    }
    return { program, std::move(arguments) };
}

} // namespace

std::string repository_root() {
    if (auto found = upward(base::normalize_path(base::current_directory()))) return *found;
    const auto arguments = os::env::arguments();
    std::string self { arguments.empty() ? std::string {} : arguments.front() };
    if (!self.empty()) {
        if (!base::is_absolute_path(self)) self = base::join_path(base::current_directory(), self);
        if (auto found = upward(base::parent_path(base::normalize_path(self)))) return *found;
    }
    return base::current_directory();
}

bool step(std::string_view what, const std::string& program, std::vector<std::string> arguments,
          const std::string& directory) {
    std::println("  {} ...", what);
    auto [toRun, toPass] = runnable(program, std::move(arguments));
    base::log::debug("{} {}", toRun, base::join(toPass, " "));
    const auto started = std::chrono::steady_clock::now();
    auto result = mcppls::toolrun::run({
        .program = toRun,
        .arguments = std::move(toPass),
        .workDirectory = directory,
        .purpose = "devtools",
        .network = mcppls::toolrun::Network::allowed,   // packaging fetches clangd and npm packages
        .bounds = mcpplibs::os::process::bounds { .hard = std::chrono::hours { 2 } },
    });
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
    if (!result) {
        std::println(std::cerr, "  {} could not start: {}", program, result.error().message);
        return false;
    }
    if (!result->status.success() || result->timed_out) {
        std::println(std::cerr, "  {} failed ({}) after {} s", what, toolrun::exit_code(result->status), seconds);
        const std::string tail { base::last_lines(result->err.empty() ? result->out : result->err, 30) };
        if (!tail.empty()) std::println(std::cerr, "{}", tail);
        return false;
    }
    std::println("  {} took {} s", what, seconds);
    return true;
}

base::Result<std::string> capture(const std::string& program, std::vector<std::string> arguments,
                                  const std::string& directory, std::chrono::milliseconds bound) {
    auto [toRun, toPass] = runnable(program, std::move(arguments));
    auto result = mcppls::toolrun::run({
        .program = toRun,
        .arguments = std::move(toPass),
        .workDirectory = directory,
        .purpose = "devtools",
        .network = mcppls::toolrun::Network::allowed,
        .bounds = mcpplibs::os::process::bounds { .hard = bound },
    });
    if (!result) return std::unexpected { result.error() };
    if (!result->status.success() || result->timed_out) {
        return base::fail("devtools-run", std::format("{} exited {}{}\n{}", program, toolrun::exit_code(result->status),
                                                      result->timed_out ? " (timed out)" : "",
                                                      base::last_lines(result->err, 30)));
    }
    return std::move(result->out);
}

std::optional<std::string> tool(std::string_view name, std::string_view whatItIsFor) {
    if (auto found = toolrun::find_program(name)) return *found;
    std::println(std::cerr, "mcppls-devtools: {} is not on PATH — {}.", name, whatItIsFor);
    std::println(std::cerr, "  It is declared in mcpp.toml's [xlings.workspace]; running this "
                            "through `mcpp run -p devtools -- ...` provisions it.");
    return std::nullopt;
}

namespace {

// Every artifact `mcpp pack --message-format json` reports.
base::Result<std::vector<std::string>> packed_artifacts(const std::string& mcpp, const std::string& root, std::vector<std::string> arguments) {
    arguments.insert(arguments.end(), { "--message-format", "json" });
    std::println("  mcpp {}", base::join(arguments, " "));
    auto output = capture(mcpp, arguments, root, std::chrono::hours { 1 });
    if (!output) return std::unexpected { output.error() };
    const auto envelope = nlohmann::json::parse(*output, nullptr, false);
    const auto artifacts = envelope.is_object() ? envelope.value("data", nlohmann::json::object()).value("artifacts", nlohmann::json::array())
                                                : nlohmann::json::array();
    std::vector<std::string> paths;
    for (const auto& artifact : artifacts) {
        if (artifact.is_object() && artifact.contains("path") && artifact["path"].is_string()) paths.push_back(artifact["path"].get<std::string>());
    }
    if (paths.empty()) return base::fail("devtools-mcpp", std::format("mcpp {} reported no artifact", base::join(arguments, " ")));
    return paths;
}

} // namespace

base::Result<std::string> server_tree(std::string_view given, const std::string& scratch) {
    std::error_code ignored;
    if (std::filesystem::is_directory(given, ignored)) return std::string { given };
    if (!std::filesystem::is_regular_file(given, ignored)) return base::fail("devtools-server", std::format("no server tree at {}", given));
    std::filesystem::remove_all(scratch, ignored);
    auto extracted = pack::archive::extract(given, scratch, [](std::string_view) { return true; });
    if (!extracted) return std::unexpected { extracted.error() };
    return scratch;
}

base::Result<ServerParts> pack_server(const std::string& root, std::string_view profile) {
    std::error_code ignored;
    auto mcpp = toolrun::find_program("mcpp");
    if (!mcpp) return base::fail("devtools-mcpp", "mcpp is not on PATH, so the server cannot be built; pass --server-image and --server-tree");
    std::println("  building the server: one okb of every architecture, and the tree with the runner of every system");
    const std::string out { base::join_path(root, "target/pack/server") };
    std::filesystem::remove_all(out, ignored);
    (void)std::filesystem::create_directories(out, ignored);
    auto artifacts = packed_artifacts(*mcpp, root, { "pack", "--profile", std::string { profile }, "--target", "openkal", "--mode", "self-contained",
                                                     "--systems", "all", "--pkg-format", "zip,okb", "-o", out + "/" });
    if (!artifacts) return std::unexpected { artifacts.error() };
    ServerParts parts;
    for (const auto& path : *artifacts) {
        if (path.ends_with(".okb")) parts.image = path;
        else if (path.ends_with(".zip")) parts.archive = path;
    }
    if (parts.image.empty() || parts.archive.empty()) {
        return base::fail("devtools-mcpp", std::format("mcpp pack wrote {}, not an okb and a zip", base::join(*artifacts, ", ")));
    }
    auto tree = server_tree(parts.archive, base::join_path(root, "target/pack/server-tree"));
    if (!tree) return std::unexpected { tree.error() };
    parts.tree = *tree;
    return parts;
}

void print_json(const nlohmann::json& value) { std::println("{}", value.dump(2)); }

} // namespace mcppls::devtools
