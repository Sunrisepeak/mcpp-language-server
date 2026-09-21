module mcppls.platform.toolenv;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.log;
import mcppls.base.text;
import mcppls.platform.env;
import mcppls.platform.process;

namespace mcppls::platform::toolenv {

namespace {

using Json = nlohmann::json;

// Variables of the editor process that say where the editor is, not where the user is. Taking
// the shell's copy of these would move the tool into the shell's idea of the session instead.
constexpr std::array SKIPPED_EXACT {
    std::string_view { "SHLVL" }, std::string_view { "PWD" }, std::string_view { "OLDPWD" },
    std::string_view { "_" },
};
constexpr std::array SKIPPED_PREFIXES {
    std::string_view { "TERM" }, std::string_view { "VSCODE_" }, std::string_view { "ELECTRON_" },
    std::string_view { "MCPPLS_" },
};

bool skipped(std::string_view name) {
    if (std::ranges::find(SKIPPED_EXACT, name) != SKIPPED_EXACT.end()) return true;
    return std::ranges::any_of(SKIPPED_PREFIXES, [&](std::string_view prefix) { return name.starts_with(prefix); });
}

std::pair<std::string_view, std::string_view> split_variable(std::string_view variable) {
    const auto equals = variable.find('=');
    if (equals == std::string_view::npos) return { variable, std::string_view {} };
    return { variable.substr(0, equals), variable.substr(equals + 1) };
}

struct State {
    std::mutex mutex;
    std::condition_variable done;
    Mode mode { Mode::automatic };
    std::string self;
    std::string shell;
    bool started { false };
    bool finished { false };
    Environment environment;
};

State& state() {
    static State value;
    return value;
}

Environment editor_environment(std::string reason) {
    Environment environment;
    environment.variables = env::variables();
    environment.source = "editor";
    environment.reason = std::move(reason);
    environment.resolved = true;
    return environment;
}

std::string login_shell(const std::string& configured) {
    if (!configured.empty()) return configured;
    if (auto shell = env::get("SHELL"); shell && !shell->empty()) return *shell;
    return "/bin/sh";
}

// A path inside a single-quoted shell word.
std::string single_quoted(std::string_view text) {
    std::string quoted { "'" };
    for (const char character : text) {
        if (character == '\'') quoted += "'\\''";
        else quoted += character;
    }
    quoted += '\'';
    return quoted;
}

Environment resolve_from_login_shell(const std::string& self, const std::string& configuredShell) {
    const auto started = std::chrono::steady_clock::now();
    if (self.empty()) return editor_environment("the server does not know its own executable");
    const std::string shell { login_shell(configuredShell) };
    const std::string marker { std::format("MCPPLS-ENVIRONMENT-{}", std::chrono::steady_clock::now().time_since_epoch().count()) };

    auto environment = env::variables();
    // The same two a shell configuration can test to skip its heavy half; VS Code sets the first.
    environment.push_back("VSCODE_RESOLVING_ENVIRONMENT=1");
    environment.push_back("MCPPLS_RESOLVING_ENVIRONMENT=1");

    SpawnOptions options {
        .program = shell,
        .arguments = { "-l", "-i", "-c", std::format("{} print-environment {}", single_quoted(self), marker) },
        .environment = environment,
    };
    // A shell configuration that waits for input, or for the network, must not keep the server
    // from starting a build tool: ten seconds, then this process's own environment.
    auto result = run(std::move(options), RunBounds { .hard = std::chrono::seconds { 10 },
                                                      .drain = std::chrono::milliseconds { 500 },
                                                      .outputLimit = 4 * 1024 * 1024,
                                                      .errorLimit = 256 * 1024 });
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (!result) {
        auto answer = editor_environment(std::format("the login shell {} could not be started: {}", shell, result.error().message));
        answer.duration = duration;
        return answer;
    }
    if (result->timedOut) {
        auto answer = editor_environment(std::format("the login shell {} did not finish within 10 s", shell));
        answer.duration = duration;
        return answer;
    }
    auto parsed = parse_marked(result->output, marker);
    if (!parsed) {
        auto answer = editor_environment(std::format("the login shell {} did not report an environment (exit {})", shell, result->exitCode));
        answer.duration = duration;
        return answer;
    }

    Environment answer;
    answer.variables = merge(env::variables(), *parsed, &answer.differing);
    answer.source = "login-shell";
    answer.duration = duration;
    answer.resolved = true;
    return answer;
}

void resolve_now() {
    auto& value = state();
    Mode mode {};
    std::string self;
    std::string shell;
    {
        std::lock_guard lock { value.mutex };
        mode = value.mode;
        self = value.self;
        shell = value.shell;
    }
    Environment environment;
    if (mode == Mode::editor) {
        environment = editor_environment("mcppls.toolEnvironment is editor");
    } else if constexpr (os::FAMILY == os::Family::windows) {
        // A graphical program on Windows takes its environment from the registry, which is what a
        // terminal takes too. There is no login shell to ask.
        environment = editor_environment("this process's environment (Windows)");
    } else {
        environment = resolve_from_login_shell(self, shell);
    }
    {
        std::lock_guard lock { value.mutex };
        value.environment = std::move(environment);
        value.finished = true;
    }
    value.done.notify_all();
    const auto answer = get();
    if (answer.source == "login-shell") {
        base::log::info("tool environment from the login shell in {} ms; {} variables differ from this process's",
                        answer.duration.count(), answer.differing.size());
    } else {
        base::log::info("tool environment is this process's: {}", answer.reason);
    }
}

} // namespace

std::optional<Mode> parse_mode(std::string_view name) {
    if (name == "auto" || name == "automatic") return Mode::automatic;
    if (name == "editor") return Mode::editor;
    return std::nullopt;
}

std::string_view mode_name(Mode mode) { return mode == Mode::editor ? "editor" : "auto"; }

void configure(Mode mode, std::string self, std::string shell) {
    auto& value = state();
    std::lock_guard lock { value.mutex };
    value.mode = mode;
    value.self = std::move(self);
    value.shell = std::move(shell);
}

Mode mode() {
    auto& value = state();
    std::lock_guard lock { value.mutex };
    return value.mode;
}

void begin() {
    auto& value = state();
    {
        std::lock_guard lock { value.mutex };
        if (value.started) return;
        value.started = true;
        value.finished = false;
    }
    std::thread { resolve_now }.detach();
}

Environment get(std::chrono::milliseconds wait) {
    auto& value = state();
    std::unique_lock lock { value.mutex };
    if (!value.started) {
        // Nobody asked for it yet: answer with this process's environment rather than block here.
        Environment answer;
        answer.variables = env::variables();
        answer.source = "editor";
        answer.reason = "the tool environment has not been resolved yet";
        answer.resolved = false;
        return answer;
    }
    if (!value.finished && wait > std::chrono::milliseconds { 0 }) {
        value.done.wait_for(lock, wait, [&] { return value.finished; });
    }
    if (!value.finished) {
        Environment answer;
        answer.variables = env::variables();
        answer.source = "editor";
        answer.reason = "the login shell is still being read";
        answer.resolved = false;
        return answer;
    }
    return value.environment;
}

void reset() {
    auto& value = state();
    std::lock_guard lock { value.mutex };
    value.started = false;
    value.finished = false;
    value.environment = Environment {};
}

std::string print_environment_text(std::string_view marker) {
    Json object = Json::object();
    for (const auto& variable : env::variables()) {
        const auto [name, text] = split_variable(variable);
        if (name.empty()) continue;
        object[std::string { name }] = std::string { text };
    }
    return std::format("{}\n{}\n{}\n", marker, object.dump(), marker);
}

std::vector<std::string> merge(const std::vector<std::string>& base, const std::vector<std::string>& fromShell,
                               std::vector<std::string>* differing) {
    std::vector<std::string> merged { base };
    auto position_of = [&](std::string_view name) -> std::optional<std::size_t> {
        for (std::size_t index { 0 }; index < merged.size(); ++index) {
            const auto [existing, unused] = split_variable(merged[index]);
            (void) unused;
            const bool same { os::CASE_INSENSITIVE_PATHS
                                  ? base::iequals_ascii(existing, name)
                                  : existing == name };
            if (same) return index;
        }
        return std::nullopt;
    };
    for (const auto& variable : fromShell) {
        const auto [name, value] = split_variable(variable);
        if (name.empty() || skipped(name)) continue;
        const auto at = position_of(name);
        if (at) {
            if (merged[*at] == variable) continue;
            merged[*at] = variable;
        } else {
            merged.push_back(variable);
        }
        if (differing) differing->push_back(std::string { name });
    }
    return merged;
}

std::optional<std::vector<std::string>> parse_marked(std::string_view text, std::string_view marker) {
    const auto first = text.find(marker);
    if (first == std::string_view::npos) return std::nullopt;
    const auto bodyStart = text.find('\n', first);
    if (bodyStart == std::string_view::npos) return std::nullopt;
    const auto second = text.find(marker, bodyStart);
    if (second == std::string_view::npos) return std::nullopt;
    const std::string_view body { text.substr(bodyStart + 1, second - bodyStart - 1) };
    Json parsed = Json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    std::vector<std::string> variables;
    for (auto entry : parsed.items()) {
        if (!entry.value().is_string()) continue;
        variables.push_back(std::format("{}={}", entry.key(), entry.value().get<std::string>()));
    }
    return variables;
}

} // namespace mcppls::platform::toolenv
