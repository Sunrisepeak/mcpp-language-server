module mcppls.cli.options;

import std;
import mcpplibs.cmdline;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native;
import mcppls.engine.native.index;
import mcppls.engine.clangd;
import mcppls.orchestrator.workspace;
import mcppls.ai.model.source;

namespace mcppls::cli {

using namespace mcpplibs;

orchestrator::EngineFactories engine_factories(const orchestrator::SessionOptions& options, const engine::PayloadPaths& payload, bool payloadCorrupt) {
    orchestrator::EngineFactories factories;
    factories.modules = [](const index::ModuleIndex& index) { return engine::native::make_engine(index); };
    if (options.engine == "none") return factories;
    if (options.engine != "clangd") base::log::warning("unknown engine {}; using clangd", options.engine);
    factories.core = [options, payload, payloadCorrupt]() -> std::unique_ptr<engine::Engine> {
        engine::clangd::Options clangd;
        clangd.executable = payload.clangd;
        clangd.version = payload.clangdVersion;
        clangd.payloadCorrupt = payloadCorrupt;
        clangd.verboseLog = options.verboseEngineLog;
        clangd.requestTimeout = options.requestTimeout;
        return engine::clangd::make_engine(std::move(clangd));
    };
    return factories;
}

orchestrator::SessionOptions session_options(const cmdline::ParsedArgs& args) {
    orchestrator::SessionOptions options;
    options.payloadDirectory = args.value("payload").value_or("");
    options.clangd = args.value("clangd").value_or("");
    options.kit = args.value("kit").value_or("");
    options.mcpp = args.value("mcpp").value_or("");
    options.database = args.value("database").value_or("");
    options.trusted = !args.is_flag_set("untrusted");
    options.discoverCompilers = !args.is_flag_set("no-discover");
    options.verboseEngineLog = args.value("log-level").value_or("") == "debug";
    if (auto chosen = args.value("engine")) {
        options.engine = *chosen;
        options.engineFromCommandLine = true;
    }
    options.engineFactories = engine_factories;
    // This very program, for the reviews an editor asks for: named as the process started it, else found on PATH.
    if (const auto arguments = platform::env::arguments(); !arguments.empty()) {
        const std::string started { arguments.front() };
        const bool hasDirectory { started.find('/') != std::string::npos || started.find('\\') != std::string::npos };
        options.serverExecutable = hasDirectory ? absolute(started) : platform::env::find_executable(started).value_or("");
    }
    if (auto buildTool = args.value("build-tool"); buildTool && (*buildTool == "offline" || *buildTool == "online" || *buildTool == "off")) {
        options.buildTool = *buildTool;
    }
    if (auto environment = args.value("tool-environment"); environment && (*environment == "auto" || *environment == "editor")) {
        options.toolEnvironment = *environment;
    }
    // Design 4.2 sets this at a minute. A machine whose build tool is honestly slower needs it
    // longer, and a test that means to watch the bound fire needs it much shorter.
    options.producerTimeout = seconds_option(args, "producer-timeout", std::chrono::seconds { 0 });
    options.requestTimeout = seconds_option(args, "request-timeout", options.requestTimeout.count() > 0
                                                                          ? std::chrono::duration_cast<std::chrono::seconds>(options.requestTimeout)
                                                                          : std::chrono::seconds { 60 });
    return options;
}

void apply_log_level(const cmdline::ParsedArgs& args) {
    if (auto level = args.value("log-level")) {
        if (auto parsed = base::log::parse_level(*level)) base::log::set_level(*parsed);
    }
}

std::string absolute(std::string_view path) {
    if (path.empty() || base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(platform::fs::current_directory(), path);
}

std::optional<ai::model::ModelSettings> model_settings(const cmdline::ParsedArgs& args, std::string_view sourceOption) {
    ai::model::ModelSettings settings;
    const std::string source { args.value(sourceOption).value_or("none") };
    const auto parsed = ai::model::parse_source(source);
    if (!parsed) return std::nullopt;
    settings.source = *parsed;
    settings.explicitlyEnabled = settings.source != ai::model::SourceKind::none;
    settings.gatewayExecutable = args.value("model-gateway") ? absolute(*args.value("model-gateway")) : std::string {};
    settings.model = args.value("model-name").value_or("");
    if (auto budget = args.value("model-budget")) {
        try {
            settings.tokenBudget = static_cast<std::size_t>(std::max(100, std::stoi(*budget)));
        } catch (...) {
        }
    }
    settings.excludedPaths = args.option_or_empty("model-exclude").values;
    return settings;
}

void add_model_options(cmdline::App& command, std::string_view sourceOption, std::string_view sourceHelp) {
    (void)command.option(sourceOption).takes_value().help(sourceHelp);
    (void)command.option("model-gateway").takes_value().help("The model gateway executable (default: mcppls-model on PATH)");
    (void)command.option("model-name").takes_value().help("The model the gateway asks");
    (void)command.option("model-budget").takes_value().help("Tokens a review may send at most (default 8000)");
    (void)command.option("model-exclude").takes_value().multiple().help("A glob of files never sent to a model; repeatable");
}

std::chrono::seconds seconds_option(const cmdline::ParsedArgs& args, std::string_view name, std::chrono::seconds fallback) {
    auto text = args.value(name);
    if (!text) return fallback;
    try {
        return std::chrono::seconds { std::stoi(*text) };
    } catch (...) {
        return fallback;
    }
}

} // namespace mcppls::cli
