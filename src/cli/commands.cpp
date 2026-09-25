module mcppls.cli.commands;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.version;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.platform.process;
import mcppls.platform.toolenv;
import mcppls.platform.toolrun;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.scan;
import mcppls.project.detect;
import mcppls.project.infer;
import mcppls.project.model;
import mcppls.normalize.plan;
import mcppls.engine;
import mcppls.engine.payload;
import mcppls.engine.native;
import mcppls.engine.native.index;
import mcppls.engine.clangd;
import mcppls.server.session;
import mcppls.cli.options;
import mcppls.cli.query;
import mcppls.cli.cache;
import mcppls.orchestrator.report;
import mcppls.orchestrator.kernel;
import mcppls.bundle.writer;
import mcppls.ai.mcp.server;
import mcppls.ai.mcp.daemon;
import mcppls.ai.model.source;

namespace mcppls::cli {

namespace {

using Json = nlohmann::json;
using namespace mcpplibs;

struct Loaded {
    engine::PayloadPaths payload;
    std::optional<spec::Kit> kit;
    project::ProjectModel model;
    normalize::EnginePlan plan;
};

// The root for a file: the nearest directory with a build description, else the file's directory.
std::string root_for(std::string_view file) {
    std::string directory { base::parent_path(file) };
    while (true) {
        for (std::string_view marker : { "mcpp.toml", "CMakeLists.txt", "compile_commands.json" }) {
            if (platform::fs::is_regular_file(base::join_path(directory, marker))) return directory;
        }
        const std::string parent { base::parent_path(directory) };
        if (parent == directory) return base::parent_path(file);
        directory = parent;
    }
}

Loaded load(std::string_view root, const cmdline::ParsedArgs& args, bool trusted) {
    Loaded loaded;
    loaded.payload = engine::resolve_payload(engine::PayloadRequest { args.value("payload").value_or(""), args.value("clangd").value_or(""), args.value("kit").value_or("") });
    if (!loaded.payload.kit.empty()) {
        if (auto kit = spec::load_kit(loaded.payload.kit)) loaded.kit = std::move(*kit);
        else base::log::warning("semantic kit unusable: {}", kit.error().message);
    }
    toolchain::ProbeCache cache { base::join_path(platform::dirs::cache_directory(), "toolchains/probe.json") };
    project::LoadOptions options;
    options.trusted = trusted;
    options.cacheDirectory = base::join_path(platform::dirs::cache_directory(), base::join_path("workspaces", project::workspace_key(root)));
    options.kit = loaded.kit ? &*loaded.kit : nullptr;
    options.runner = toolchain::process_runner(std::chrono::seconds { 20 });
    options.probeCache = &cache;
    options.discoverCompilers = !args.is_flag_set("no-discover");
    options.mcppExecutable = args.value("mcpp").value_or("");
    options.configuredDatabase = args.value("database").value_or("");
    loaded.model = project::load_project(root, options);

    normalize::PlanInput input;
    input.database = &loaded.model.database;
    input.facts = &loaded.model.facts;
    input.kit = options.kit;
    input.engineDriverDirectory = loaded.payload.clangd.empty() ? std::string {} : base::parent_path(loaded.payload.clangd);
    input.macosSdk = options.kit && spec::requires_macos_sdk(*options.kit) ? engine::macos_sdk_path() : std::string {};
    input.scanner = project::file_scanner();
    input.metadataReader = spec::caching_metadata_reader();
    loaded.plan = normalize::plan_engine(input);
    return loaded;
}

int command_model(const cmdline::ParsedArgs& args) {
    const std::string root { absolute(args.value("root").value_or(platform::fs::current_directory())) };
    const Loaded loaded { load(root, args, !args.is_flag_set("untrusted")) };
    const std::string format { args.value("export").value_or("s1") };
    if (format == "compile-commands") {
        // S1 section 12: the export keeps the default context's command per file and loses the rest.
        std::println(std::cerr, "note: compile_commands.json has no module graph, toolchain or role information, and one set per file");
        std::println("{}", spec::to_compile_commands(loaded.model.database).dump(2));
    } else if (format == "engine") {
        std::println("{}", normalize::to_compile_commands(loaded.plan).dump(2));
    } else if (format == "s1") {
        std::println("{}", spec::to_json(loaded.model.database).dump(2));
    } else {
        std::println(std::cerr, "unknown export format {}; use s1, compile-commands or engine", format);
        return 2;
    }
    return 0;
}

int command_check(const cmdline::ParsedArgs& args) {
    const std::string file { absolute(args.value("file").value_or("")) };
    if (file.empty() || !platform::fs::is_regular_file(file)) {
        std::println(std::cerr, "check: no such file: {}", file);
        return 2;
    }
    const std::string root { args.value("root") ? absolute(*args.value("root")) : root_for(file) };
    const Loaded loaded { load(root, args, !args.is_flag_set("untrusted")) };
    const auto& model = loaded.model;
    std::println("root      {}", root);
    // `tier` (README L1..L4, S3-4-8) is which kind of source this came from; `level` is S1's own
    // document-conformance number (S1 §7.1). They are printed separately so neither reads as the
    // other's number.
    std::println("source    {} (tier {}, level {})", project::to_string(model.source), model.tier, model.level);
    if (!model.producer.empty()) std::println("producer  {}{}", model.producer, model.producerVersion.empty() ? std::string {} : std::format(" {}", model.producerVersion));
    std::println("profile   {} {} {} {}", model.profile.kind, model.profile.compiler, model.profile.stdlib, model.profile.target);
    std::println("engine    clangd {} {}", loaded.payload.clangdVersion, loaded.payload.clangd);
    std::println("database  {} entries, {} standard library units, {} left out", loaded.plan.entries.size(), loaded.plan.stdUnits, loaded.plan.excludedFiles.size());
    for (const auto& issue : model.issues) std::println("issue     [{}] {}", issue.code, issue.message);
    for (const auto& notice : model.notices) std::println("notice    [{}] {}", notice.code, notice.message);
    for (const auto& issue : loaded.plan.issues) std::println("issue     [{}] {} ({})", issue.code, issue.message, issue.file);

    index::ModuleIndex index;
    for (const auto& set : model.database.sets) {
        for (const auto& unit : set.units) {
            const std::string path { spec::absolute_source(unit) };
            if (auto text = platform::fs::read_file(path)) index.update(path, *text);
        }
    }
    if (auto text = platform::fs::read_file(file)) index.update(file, *text);
    std::vector<std::pair<std::string, std::string>> manifests;
    for (auto& manifest : project::module_manifests(model, loaded.kit ? &*loaded.kit : nullptr)) manifests.emplace_back(manifest.path, manifest.origin);
    index.set_external(index::external_modules(manifests, spec::caching_metadata_reader()));
    for (const auto& diagnostic : index.diagnostics(file)) {
        std::println("module    {}:{}: {}", base::file_name(file), diagnostic["range"]["start"]["line"].get<int>() + 1, diagnostic.value("message", std::string {}));
    }
    if (loaded.payload.clangd.empty() || !platform::fs::is_regular_file(loaded.payload.clangd)) {
        std::println("clangd    not found; skipped the semantic check");
        return loaded.plan.issues.empty() ? 0 : 1;
    }
    const std::string directory { base::join_path(platform::dirs::temp_directory(), std::format("mcppls-check-{}", project::workspace_key(root))) };
    if (auto written = normalize::write_engine_database(directory, loaded.plan); !written) {
        std::println(std::cerr, "check: {}", written.error().message);
        return 2;
    }
    auto result = platform::toolrun::run({
        .program = loaded.payload.clangd,
        .arguments = { "--check=" + file, "--experimental-modules-support", "--compile-commands-dir=" + directory, "--log=error" },
        .workDirectory = root,
        .purpose = "check",
        .root = root,
        .bounds = platform::RunBounds { .hard = std::chrono::minutes { 5 } },
    });
    if (!result) {
        std::println(std::cerr, "check: {}", result.error().message);
        return 2;
    }
    std::print("{}", result->error);
    std::println("clangd    exit {}{}", result->exitCode, result->timedOut ? " (timed out)" : "");
    return result->exitCode == 0 && !result->timedOut ? 0 : 1;
}

// The options a daemon started for an entry is started with: the entry's own.
std::vector<std::string> daemon_arguments(const cmdline::ParsedArgs& args) {
    std::vector<std::string> forwarded;
    for (const std::string_view name : { "payload", "clangd", "kit", "mcpp", "database", "engine", "request-timeout", "log-level", "tool-timeout",
                                         "model-source", "model-gateway", "model-name", "model-budget", "idle-minutes" }) {
        if (auto value = args.value(name)) forwarded.insert(forwarded.end(), { std::format("--{}", name), *value });
    }
    for (const auto& pattern : args.option_or_empty("model-exclude").values) forwarded.insert(forwarded.end(), { "--model-exclude", pattern });
    for (const auto& id : args.option_or_empty("disable-workaround").values) forwarded.insert(forwarded.end(), { "--disable-workaround", id });
    for (const std::string_view flag : { "untrusted", "no-discover" }) {
        if (args.is_flag_set(flag)) forwarded.push_back(std::format("--{}", flag));
    }
    return forwarded;
}

} // namespace

int run(int argc, char* argv[]) {
    int status { 0 };
    bool handled { false };
    auto serve = [&](const cmdline::ParsedArgs& args) {
        handled = true;
        apply_log_level(args);
        // robustness design O2: the session's log outlives the editor's output.
        if (const std::string logFile { orchestrator::open_log_file("server") }; !logFile.empty()) base::log::info("log file {}", logFile);
        status = server::run_session(session_options(args));
    };

    // Built statement by statement: a fluent chain nests a subcommand under the
    // previous one once an option has been added to it.
    cmdline::App app { "mcppls" };
    (void)app.version(std::string { base::VERSION });
    (void)app.description("Compiler-agnostic C++ modules language server");
    (void)app.option("payload").takes_value().global(true).help("Payload directory with clangd and the semantic kit");
    (void)app.option("clangd").takes_value().global(true).help("clangd executable (overrides the payload)");
    (void)app.option("kit").takes_value().global(true).help("Semantic kit directory (overrides the payload)");
    (void)app.option("mcpp").takes_value().global(true).help("The mcpp executable for mcpp projects (default: found on PATH)");
    (void)app.option("database").takes_value().global(true).help("A workspace's own S1 build database, relative to its root");
    (void)app.option("untrusted").global(true).help("Do not run build tools or compilers");
    (void)app.option("no-discover").global(true).help("Do not look for compilers; loose sources use the semantic kit");
    (void)app.option("log-level").takes_value().global(true).help("debug | info | warning | error");
    (void)app.option("request-timeout").takes_value().global(true).help("Seconds before an engine request is answered without it");
    (void)app.option("build-tool").takes_value().global(true).help("How the project's build tool may be run: offline (default), online, off");
    (void)app.option("tool-environment").takes_value().global(true).help("Which environment build tools run in: auto (the login shell on POSIX) or editor");
    (void)app.option("producer-timeout").takes_value().global(true).help("Seconds a build tool may take to describe the project (default 60, or 600 when online)");
    (void)app.option("engine").takes_value().global(true).help("The core semantic engine: clangd (default) or none, mcppls's own module features only");
    (void)app.option("disable-workaround").takes_value().multiple().global(true).help("Turn off a registered clangd workaround (WA-CLANGD-<n>, see mcppls report); repeatable");
    // Language clients pass these by convention; this server always speaks over its standard streams.
    (void)app.option("stdio").global(true).help("Accepted for language clients; standard input and output are always used");
    (void)app.option("clientProcessId").takes_value().global(true).help("Accepted for language clients; not used");
    (void)app.action(serve);

    cmdline::App serveCommand { "serve" };
    (void)serveCommand.description("Serve the Language Server Protocol on standard input and output (the default)");
    (void)serveCommand.action(serve);
    (void)app.subcommand(std::move(serveCommand));

    cmdline::App checkCommand { "check" };
    (void)checkCommand.description("Load the project of a file, print the model, plan and diagnostics, and run clangd --check");
    (void)checkCommand.option("root").takes_value().help("Project root (default: nearest build description)");
    (void)checkCommand.arg("file").required();
    (void)checkCommand.action([&](const cmdline::ParsedArgs& args) { handled = true; status = command_check(args); });
    (void)app.subcommand(std::move(checkCommand));

    // Design 4.3: the server asks the user's login shell to run this, and reads the environment it
    // prints between the two markers. A shell configuration prints all sorts of things on startup,
    // which is why the answer is delimited rather than simply being the output.
    cmdline::App printEnvironmentCommand { "print-environment" };
    (void)printEnvironmentCommand.description("Print this process's environment as JSON between two marker lines (used to read the login shell's environment)");
    (void)printEnvironmentCommand.arg("marker").required();
    (void)printEnvironmentCommand.action([&](const cmdline::ParsedArgs& args) {
        handled = true;
        std::print("{}", platform::toolenv::print_environment_text(args.value("marker").value_or(std::string { "MCPPLS-ENVIRONMENT" })));
        status = 0;
    });
    (void)app.subcommand(std::move(printEnvironmentCommand));

    cmdline::App reportCommand { "report" };
    (void)reportCommand.description("Load a workspace, let it settle, and print what a bug report needs as JSON (robustness design O3)");
    (void)reportCommand.option("root").takes_value().help("Workspace root (default: the current directory)");
    (void)reportCommand.option("settle").takes_value().help("Seconds to wait for the engines to settle first (default 60)");
    // issue #23 fix plan F18: the diagnostic bundle, and the redaction every report goes through.
    (void)reportCommand.option("bundle").takes_value().help("Write a diagnostic bundle (a zip of the report, environment, logs, incidents and engine databases) here instead of printing the report");
    (void)reportCommand.option("hide-project-paths").help("Replace the workspace's own paths too, with <workspace>");
    (void)reportCommand.option("no-source-excerpts").help("Leave out the lines of source an incident carries");
    (void)reportCommand.option("include-dumps").help("Include crash dumps in the bundle; they hold memory and cannot be redacted");
    (void)reportCommand.option("no-redact").help("Keep user names, paths and secrets as they are, to look at a problem on this machine; never for sharing");
    (void)reportCommand.action([&](const cmdline::ParsedArgs& args) {
        handled = true;
        apply_log_level(args);
        const std::string root { args.value("root") ? absolute(*args.value("root")) : platform::fs::current_directory() };
        const auto started = std::chrono::steady_clock::now();
        orchestrator::KernelOptions options { session_options(args), root };
        auto kernel = orchestrator::Kernel::start(options);
        const auto settle = seconds_option(args, "settle", std::chrono::seconds { 60 });
        // The core engine accepting (or known unavailable) first: a report taken before its handshake says little.
        (void)kernel->wait_until([&] {
            const auto core = kernel->workspace().core_engine_status();
            return !core || core->accepting || core->state == "unavailable";
        }, settle);
        (void)kernel->wait_settled(settle);
        Json roots = Json::array({ kernel->workspace().report() });
        const engine::PayloadPaths payload { engine::resolve_payload(engine::PayloadRequest { options.session.payloadDirectory, options.session.clangd,
                                                                                               options.session.kit, options.session.engine }) };
        Json report = orchestrator::make_report(std::move(roots), Json { { "name", "mcppls report" } }, options.session.engine, payload, false,
                                                std::chrono::steady_clock::now() - started);
        const bool redact { !args.is_flag_set("no-redact") };
        if (auto output = args.value("bundle")) {
            // Before the kernel shuts down: a second instance's private cache, with its engine database, goes with it.
            bundle::BundleInput input;
            input.report = std::move(report);
            bundle::BundleOptions bundleOptions;
            bundleOptions.output = absolute(*output);
            bundleOptions.redact = redact;
            bundleOptions.hideProjectPaths = args.is_flag_set("hide-project-paths");
            bundleOptions.sourceExcerpts = !args.is_flag_set("no-source-excerpts");
            bundleOptions.includeDumps = args.is_flag_set("include-dumps");
            auto written = bundle::write_bundle(input, bundleOptions);
            kernel->shut_down();
            if (!written) {
                std::println(std::cerr, "report: {}", written.error().message);
                std::println("{}", Json { { "error", written.error().message }, { "residue", written.error().residue } }.dump(2));
                status = 1;
                return;
            }
            std::println("{}", Json { { "bundle", written->path }, { "bytes", written->bytes }, { "redactions", written->redactions } }.dump(2));
            status = 0;
            return;
        }
        kernel->shut_down();
        std::println("{}", (redact ? bundle::redact_report(report) : report).dump(2));
        status = 0;
    });
    (void)app.subcommand(std::move(reportCommand));

    cmdline::App modelCommand { "model" };
    (void)modelCommand.description("Print the project model as S1, as compile_commands.json, or the engine database");
    (void)modelCommand.option("root").takes_value().help("Project root (default: the current directory)");
    (void)modelCommand.option("export").takes_value().help("s1 | compile-commands | engine");
    (void)modelCommand.action([&](const cmdline::ParsedArgs& args) { handled = true; status = command_model(args); });
    (void)app.subcommand(std::move(modelCommand));

    cmdline::App mcpCommand { "mcp" };
    (void)mcpCommand.description("Serve the Model Context Protocol on standard input and output, for coding agents");
    (void)mcpCommand.option("root").takes_value().help("Workspace root (default: the current directory)");
    (void)mcpCommand.option("tool-timeout").takes_value().help("Seconds a tool call waits for the engines (default 120)");
    (void)mcpCommand.option("daemon").help("Share the workspace daemon's warm session, starting it when none runs");
    add_model_options(mcpCommand, "model-source", "The model cxx_review may use besides the agent: gateway or mcp-sampling (default none)");
    (void)mcpCommand.action([&](const cmdline::ParsedArgs& args) {
        handled = true;
        // Standard output carries the protocol; the log goes to standard error, warnings only unless asked.
        if (!args.value("log-level")) base::log::set_level(base::log::Level::warning);
        apply_log_level(args);
        const auto model = model_settings(args, "model-source");
        if (!model) {
            std::println(std::cerr, "mcp: --model-source is none, gateway or mcp-sampling");
            status = 2;
            return;
        }
        const std::string root { args.value("root") ? absolute(*args.value("root")) : platform::fs::current_directory() };
        if (args.is_flag_set("daemon")) {
            // design 6.3: the workspace's daemon serves this connection, started when there is none yet.
            std::optional<ai::mcp::DaemonInfo> daemon { ai::mcp::find_daemon(root) };
            if (daemon && !ai::mcp::control(*daemon, "status")) daemon.reset();
            if (!daemon) {
                const auto options = session_options(args);
                auto started = ai::mcp::start_daemon(options.serverExecutable, root, daemon_arguments(args), std::chrono::seconds { 60 });
                if (started) daemon = *started;
                else base::log::warning("the workspace daemon did not start ({}); serving this connection alone", started.error().message);
            }
            if (daemon) {
                status = ai::mcp::relay_mcp(*daemon);
                return;
            }
        }
        if (const std::string logFile { orchestrator::open_log_file("mcp") }; !logFile.empty()) base::log::info("log file {}", logFile);
        status = ai::mcp::run_server(ai::mcp::ServerOptions { orchestrator::KernelOptions { session_options(args), root },
                                                              seconds_option(args, "tool-timeout", std::chrono::seconds { 120 }), *model });
    });
    (void)app.subcommand(std::move(mcpCommand));

    cmdline::App daemonCommand { "daemon" };
    (void)daemonCommand.description("The workspace daemon that MCP connections share: run, start, status, stop");
    auto daemonActions = std::make_shared<std::map<std::string, std::function<void(const cmdline::ParsedArgs&)>, std::less<>>>();
    (void)daemonCommand.action([daemonActions, &handled, &status](const cmdline::ParsedArgs& args) {
        const auto sub = args.subcommand();
        const auto found = daemonActions->find(args.subcommand_name());
        handled = true;
        if (!sub || found == daemonActions->end()) {
            std::println(std::cerr, "daemon: run, start, status or stop (mcppls daemon --help)");
            status = 2;
            return;
        }
        found->second(sub->get());
    });
    const auto daemonRoot = [](const cmdline::ParsedArgs& args) {
        return args.value("root") ? absolute(*args.value("root")) : platform::fs::current_directory();
    };
    const auto addDaemon = [&](std::string name, std::string_view description, std::function<void(const cmdline::ParsedArgs&)> action) {
        cmdline::App sub { name };
        (void)sub.description(description);
        (void)sub.option("root").takes_value().help("Workspace root (default: the current directory)");
        if (name == "run" || name == "start") {
            (void)sub.option("tool-timeout").takes_value().help("Seconds a tool call waits for the engines (default 120)");
            (void)sub.option("idle-minutes").takes_value().help("Minutes without a connection before the daemon exits (default 30)");
            add_model_options(sub, "model-source", "The model cxx_review may use besides the agent: gateway or mcp-sampling (default none)");
        }
        (void)sub.action(action);
        (*daemonActions)[name] = std::move(action);
        (void)daemonCommand.subcommand(std::move(sub));
    };
    addDaemon("run", "Serve as the workspace daemon in the foreground", [&, daemonRoot](const cmdline::ParsedArgs& args) {
        if (!args.value("log-level")) base::log::set_level(base::log::Level::info);
        apply_log_level(args);
        const auto model = model_settings(args, "model-source");
        ai::mcp::DaemonOptions options;
        options.server = ai::mcp::ServerOptions { orchestrator::KernelOptions { session_options(args), daemonRoot(args) },
                                                  seconds_option(args, "tool-timeout", std::chrono::seconds { 120 }), model.value_or(ai::model::ModelSettings {}) };
        options.idle = std::chrono::minutes { std::max<std::int64_t>(1, seconds_option(args, "idle-minutes", std::chrono::seconds { 30 }).count()) };
        status = ai::mcp::run_daemon(options);
    });
    addDaemon("start", "Start the workspace daemon in the background, unless one serves the workspace", [&, daemonRoot](const cmdline::ParsedArgs& args) {
        const std::string root { daemonRoot(args) };
        if (auto running = ai::mcp::find_daemon(root); running && ai::mcp::control(*running, "status")) {
            std::println("{}", nlohmann::json { { "port", running->port }, { "root", running->root }, { "started", false } }.dump());
            status = 0;
            return;
        }
        auto started = ai::mcp::start_daemon(session_options(args).serverExecutable, root, daemon_arguments(args), std::chrono::seconds { 60 });
        if (!started) {
            std::println(std::cerr, "daemon start: {}", started.error().message);
            status = 2;
            return;
        }
        std::println("{}", nlohmann::json { { "port", started->port }, { "root", started->root }, { "started", true } }.dump());
        status = 0;
    });
    for (const std::string_view method : { "status", "stop" }) {
        addDaemon(std::string { method }, method == "status" ? "Print the workspace daemon's state" : "Stop the workspace daemon",
                  [&, daemonRoot, method](const cmdline::ParsedArgs& args) {
            const auto daemon = ai::mcp::find_daemon(daemonRoot(args));
            if (!daemon) {
                std::println(std::cerr, "no daemon serves {}", daemonRoot(args));
                status = 1;
                return;
            }
            auto answer = ai::mcp::control(*daemon, method);
            if (!answer) {
                std::println(std::cerr, "daemon {}: {}", method, answer.error().message);
                status = 2;
                return;
            }
            std::println("{}", answer->dump(2));
            status = 0;
        });
    }
    (void)app.subcommand(std::move(daemonCommand));

    (void)app.subcommand(query_command(handled, status));
    (void)app.subcommand(diagnostics_command(handled, status));
    (void)app.subcommand(verify_command(handled, status));
    (void)app.subcommand(impact_command(handled, status));
    (void)app.subcommand(review_command(handled, status));
    (void)app.subcommand(cache_command(handled, status));

    cmdline::App versionCommand { "version" };
    (void)versionCommand.description("Print the version");
    (void)versionCommand.action([&](const cmdline::ParsedArgs&) {
        handled = true;
        std::println("mcppls {} ({}; S1 {}; clangd {})", base::VERSION, mcppls::os::PLATFORM, spec::PROFILE_VERSION, base::CLANGD_VERSION);
    });
    (void)app.subcommand(std::move(versionCommand));
    const int parsed { app.run(argc, argv) };
    if (parsed != 0) return parsed;
    return handled ? status : 0;
}

} // namespace mcppls::cli
