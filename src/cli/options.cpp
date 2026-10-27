module mcppls.cli.options;

import std;
import mcpplibs.cmdline;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.config.settings;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native;
import mcppls.engine.native.index;
import mcppls.engine.clangd;
import mcppls.orchestrator.cache;
import mcppls.orchestrator.workspace;
import mcppls.ai.model.source;
import mcpplibs.os;
import mcppls.toolrun;

namespace os = mcpplibs::os;

namespace mcppls::cli {

using namespace mcpplibs;

orchestrator::EngineFactories engine_factories(const orchestrator::SessionOptions& options, const engine::PayloadPaths& payload, bool payloadCorrupt) {
    orchestrator::EngineFactories factories;
    // initializationOptions.semanticTokens (design doc 2026-09-25 K/§7, contract T0).
    const engine::native::TokenOptions tokenOptions { options.semanticTokensModules, options.semanticTokensModuleType };
    factories.modules = [tokenOptions](const index::ModuleIndex& index) { return engine::native::make_engine(index, tokenOptions); };
    if (options.engine == "none") return factories;
    if (options.engine != "clangd") base::log::warning("unknown engine {}; using clangd", options.engine);
    factories.core = [options, payload, payloadCorrupt]() -> std::unique_ptr<engine::Engine> {
        engine::clangd::Options clangd;
        clangd.executable = payload.clangd;
        clangd.version = payload.clangdVersion;
        if (!payloadCorrupt && payload.engineIdentity) clangd.verifiedFeatures = payload.engineIdentity->features;
        clangd.formatFallbackStyle = options.formatFallbackStyle;
        clangd.mcppFormatStyle = !payloadCorrupt && payload.engineIdentity &&
            std::ranges::find(payload.engineIdentity->features, "format-style-mcpp") != payload.engineIdentity->features.end();
        clangd.payloadCorrupt = payloadCorrupt;
        clangd.verboseLog = options.verboseEngineLog;
        // mcppls-clangd's owned module cache (Linux) admits payloads up to a bound of its own, 1 GiB; past it, builds wait for
        // room. The cache this server keeps to its budget is the workspace's (mcppls.cache.maxBytes), so the engine is
        // given three quarters of that (ux-mcpp: a profile change's modules beside the first profile's, with every
        // reader's copy, waited out admission and the files that needed them stalled). Unlimited is unbounded; the
        // option is a 32-bit count of MiB.
        if (const auto budget = orchestrator::cache::parse_bytes(options.settings.string_value("cache.maxBytes"))) {
            constexpr std::uint64_t MIB { std::uint64_t { 1 } << 20 };
            clangd.ownedCachePayloadMiB = *budget == std::numeric_limits<std::uint64_t>::max()
                ? 0 : std::clamp<std::uint64_t>(*budget / 4 * 3 / MIB, 1024, std::numeric_limits<std::uint32_t>::max());
        }
        clangd.requestTimeout = options.requestTimeout;
        if (options.engineWorkers != "auto" && !options.engineWorkers.empty()) {
            std::size_t workers { 0 };
            const std::string_view text { options.engineWorkers };
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), workers);
            if (error == std::errc {} && end == text.data() + text.size() && workers > 0) clangd.workers = workers;
            else base::log::warning("mcppls.engine.workers is '{}', neither auto nor a number; using auto", options.engineWorkers);
        }
        clangd.disabledWorkarounds = options.disabledWorkarounds;
        clangd.preambleStorage = options.preambleStorage;
        clangd.diagnosticsWhileTyping = options.diagnosticsWhileTyping == "always";
        clangd.primeImplementationUnits = options.primeImplementationUnits != "off";
        return engine::clangd::make_engine(std::move(clangd));
    };
    return factories;
}

// Every field below is read out of `options.settings` after its command-line layer (config settings
// §9 T1): the one place a flag's default, its validation and its precedence over a client are
// defined is the registry, not this function. `handle_initialize_` and `workspace/didChangeConfiguration`
// (`mcppls.server.session`) layer over the very same `Settings` object and re-derive these same
// fields the same way, through `orchestrator::Workspace::reload_with_options`.
orchestrator::SessionOptions session_options(const cmdline::ParsedArgs& args) {
    orchestrator::SessionOptions options;
    options.settings.apply_command_line(args);
    const auto& settings = options.settings;
    options.payloadDirectory = settings.string_value("payload");
    options.clangd = settings.string_value("clangd");
    options.kit = settings.string_value("kit");
    options.mcpp = settings.string_value("mcpp");
    options.database = settings.string_value("database");
    options.compiler = settings.string_value("compiler");
    options.semanticKit = settings.string_value("semanticKit");
    options.trusted = !settings.bool_value("untrusted");
    options.discoverCompilers = settings.bool_value("discoverCompilers");
    options.verboseEngineLog = settings.string_value("logLevel") == "debug";
    options.engine = settings.string_value("engine.name");
    options.engineFactories = engine_factories;
    options.disabledWorkarounds = settings.list_value("disableWorkaround");
    options.buildTool = settings.string_value("buildTool");
    options.toolEnvironment = settings.string_value("toolEnvironment");
    options.semanticTokensModules = settings.bool_value("semanticTokens.modules");
    options.semanticTokensModuleType = settings.bool_value("semanticTokens.moduleType");
    options.buildDiscovery = settings.string_value("buildDiscovery.mode");
    options.buildDiscoveryProviders = settings.list_value("buildDiscovery.providers");
    options.mcppTarget = settings.string_value("buildDiscovery.mcppTarget");
    options.buildDiscoveryAskBeforeDownload = settings.bool_value("buildDiscovery.askBeforeDownload");
    options.primeImplementationUnits = settings.string_value("index.primeImplementationUnits");
    // Design 4.2 sets this at a minute. A machine whose build tool is honestly slower needs it
    // longer, and a test that means to watch the bound fire needs it much shorter.
    options.producerTimeout = settings.seconds_value("producerTimeout");
    options.requestTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(settings.seconds_value("requestTimeout"));
    options.formatFallbackStyle = settings.string_value("format.fallbackStyle");
    options.engineWorkers = settings.string_value("engine.workers");
    options.preambleStorage = settings.string_value("engine.preambleStorage");
    options.diagnosticsWhileTyping = settings.string_value("engine.diagnosticsWhileTyping");
    options.answerCache = settings.string_value("answer.cache");
    options.answerBudget = std::chrono::milliseconds { std::clamp(std::atoi(settings.string_value("answer.budget").c_str()), 100, 2000) };
    options.answerPrefetch = settings.bool_value("answer.prefetch");
    options.answerAgent = settings.string_value("answer.agent");
    options.completionProvenance = settings.string_value("completion.provenance");
    // This very program, for the reviews an editor asks for: as the system names it (an ok image's
    // loader and image included), else as the process was started, else found on PATH.
    options.serverCommand = os::env::self_command();
    if (options.serverCommand.empty()) {
        if (const auto arguments = os::env::arguments(); !arguments.empty()) {
            const std::string started { arguments.front() };
            const bool hasDirectory { started.find('/') != std::string::npos || started.find('\\') != std::string::npos };
            if (auto found = hasDirectory ? std::optional<std::string> { absolute(started) } : toolrun::find_program(started))
                options.serverCommand = { *found };
        }
    }
    return options;
}

void apply_log_level(const cmdline::ParsedArgs& args) {
    if (auto level = args.value("log-level")) {
        if (auto parsed = base::log::parse_level(*level)) base::log::set_level(*parsed);
    }
}

std::string absolute(std::string_view path) {
    if (path.empty() || base::is_absolute_path(path)) return base::normalize_path(path);
    return base::join_path(base::current_directory(), path);
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
