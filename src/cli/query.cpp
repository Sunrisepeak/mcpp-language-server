module mcppls.cli.query;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.platform.fs;
import mcppls.spec.query;
import mcppls.orchestrator.kernel;
import mcppls.ai.query.view;
import mcppls.ai.query.symbols;
import mcppls.ai.query.files;
import mcppls.ai.query.modules;
import mcppls.ai.context.build;
import mcppls.ai.context.interface;
import mcppls.ai.verify.changes;
import mcppls.ai.verify.toolchains;
import mcppls.ai.review.pipeline;
import mcppls.ai.review.report;
import mcppls.ai.review.judgement;
import mcppls.ai.model.source;
import mcppls.ai.model.gateway;
import mcppls.platform.env;
import mcppls.orchestrator.workspace;
import mcppls.cli.options;

namespace mcppls::cli {

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace mcpplibs;
namespace query = ai::query;

constexpr int EXIT_NOTHING { 1 };
constexpr int EXIT_FAILED { 2 };

// The root of a session: --root, else the nearest directory above `file` with a build description,
// else the current directory.
std::string root_of(const cmdline::ParsedArgs& args, std::string_view file) {
    if (auto root = args.value("root")) return absolute(*root);
    if (file.empty()) return platform::fs::current_directory();
    std::string directory { base::parent_path(absolute(file)) };
    while (true) {
        for (std::string_view marker : { "mcpp.toml", "CMakeLists.txt", "compile_commands.json" }) {
            if (platform::fs::is_regular_file(base::join_path(directory, marker))) return directory;
        }
        const std::string parent { base::parent_path(directory) };
        if (parent == directory) return platform::fs::current_directory();
        directory = parent;
    }
}

struct Position {
    std::string file;
    int line { 0 };
    int column { 0 };
};

// FILE:LINE[:COLUMN]; a Windows drive letter's colon is part of the file.
std::optional<Position> parse_position(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t end { text.size() };
    for (int i { 0 }; i < 2; ++i) {
        const std::size_t colon { text.rfind(':', end - 1) };
        if (colon == std::string_view::npos || colon == 0) break;
        const std::string_view number { text.substr(colon + 1, end - colon - 1) };
        if (number.empty() || !std::ranges::all_of(number, [](char c) { return c >= '0' && c <= '9'; })) break;
        parts.insert(parts.begin(), number);
        end = colon;
    }
    if (parts.empty()) return std::nullopt;
    Position position;
    position.file = std::string { text.substr(0, end) };
    position.line = std::stoi(std::string { parts[0] });
    position.column = parts.size() > 1 ? std::stoi(std::string { parts[1] }) : 1;
    return position;
}

std::size_t max_results(const cmdline::ParsedArgs& args, std::size_t fallback) {
    if (auto text = args.value("max")) {
        try {
            return static_cast<std::size_t>(std::max(1, std::stoi(*text)));
        } catch (...) {
        }
    }
    return fallback;
}

std::string location_text(const Json& location) {
    return std::format("{}:{}:{}", location.value("file", std::string {}), location.value("line", 0), location.value("column", 0));
}

void print_text_symbols(const Json& result) {
    for (const auto& symbol : result.value("symbols", Json::array())) {
        std::println("{} {}{}", symbol.value("kind", std::string {}), symbol.value("qualifiedName", std::string {}),
                     symbol.contains("module") ? std::format("  [{}]", symbol.value("module", std::string {})) : std::string {});
        if (symbol.contains("signature")) std::println("  {}", base::replace_all(symbol.value("signature", std::string {}), "\n", "\n  "));
        if (symbol.contains("declaration")) std::println("  declared at {}", location_text(symbol["declaration"]));
        if (symbol.contains("definition")) std::println("  defined at  {}", location_text(symbol["definition"]));
        if (symbol.contains("documentation")) std::println("  {}", base::replace_all(symbol.value("documentation", std::string {}), "\n", "\n  "));
    }
    if (result.value("truncated", false)) std::println("({} of {} shown)", result.value("symbols", Json::array()).size(), result.value("total", 0));
}

void print_text_references(const Json& result) {
    std::println("{} references to {}", result.value("total", 0), result["symbol"].value("qualifiedName", std::string {}));
    for (const auto& group : result.value("groups", Json::array())) {
        std::println("{}{}", group.value("file", std::string {}), group.contains("module") ? std::format("  [{}]", group.value("module", std::string {})) : std::string {});
        for (const auto& reference : group.value("references", Json::array())) {
            std::println("  {}:{}  {}", reference.value("line", 0), reference.value("column", 0), base::trim(reference.value("text", std::string {})));
        }
    }
}

void print_text_calls(const Json& result) {
    std::println("{} of {}", result.value("direction", std::string {}), result["symbol"].value("qualifiedName", std::string {}));
    for (const auto& call : result.value("calls", Json::array())) {
        const Json& symbol = call["symbol"];
        std::println("  {} {}  {}", symbol.value("kind", std::string {}), symbol.value("qualifiedName", std::string {}),
                     symbol.contains("definition") ? location_text(symbol["definition"]) : std::string {});
        for (const auto& site : call.value("sites", Json::array())) std::println("    {}  {}", location_text(site), base::trim(site.value("text", std::string {})));
    }
}

void print_outline_entries(const Json& entries, int depth) {
    for (const auto& entry : entries) {
        std::println("{}{}:{} {} {}", std::string(static_cast<std::size_t>(depth) * 2, ' '), entry.value("line", 0), "", entry.value("kind", std::string {}),
                     entry.value("name", std::string {}));
        if (entry.contains("children")) print_outline_entries(entry["children"], depth + 1);
    }
}

void print_text_outline(const Json& result) {
    std::println("{}{}", result.value("file", std::string {}), result.contains("module") ? std::format("  [{}]", result.value("module", std::string {})) : std::string {});
    print_outline_entries(result.value("symbols", Json::array()), 1);
}

void print_text_module(const Json& result) {
    if (result.contains("modules")) {
        for (const auto& module : result.value("modules", Json::array())) std::println("{}{}", module.value("name", std::string {}), module.value("external", false) ? " (external)" : "");
        for (const auto& edge : result.value("imports", Json::array())) std::println("  {} -> {}", edge.value("from", std::string {}), edge.value("to", std::string {}));
        return;
    }
    std::println("module {}{}", result.value("name", std::string {}), result.value("external", false) ? " (external)" : "");
    for (const auto& unit : result.value("units", Json::array())) {
        std::println("  {} {} {}", unit.value("role", std::string {}), unit.value("name", std::string {}), unit.value("file", std::string {}));
    }
    auto list = [&](std::string_view key, std::string_view label) {
        const Json values = result.value(std::string { key }, Json::array());
        if (values.empty()) return;
        std::vector<std::string> names;
        for (const auto& value : values) names.push_back(value.get<std::string>());
        std::println("  {}: {}", label, base::join(names, ", "));
    };
    list("exportedPartitions", "exports");
    list("imports", "imports");
    list("importedBy", "imported by");
    list("importingFiles", "imported by files");
}

void print_text_diagnostics(const Json& result) {
    for (const auto& file : result.value("files", Json::array())) {
        for (const auto& diagnostic : file.value("diagnostics", Json::array())) {
            std::println("{}: {}: {}{}", location_text(diagnostic["location"]), diagnostic.value("severity", std::string {}), diagnostic.value("message", std::string {}),
                         diagnostic.contains("code") ? std::format(" [{}]", diagnostic.value("code", std::string {})) : std::string {});
        }
        if (!file.value("complete", true)) std::println("{}: incomplete: {}", file.value("file", std::string {}), file.value("reason", std::string {}));
    }
    const Json counts = result.value("counts", Json::object());
    std::println("{} errors, {} warnings ({})", counts.value("error", 0), counts.value("warning", 0), result.value("semanticSource", std::string {}));
}

// Runs `ask` in a headless session and prints its result.
int run_in_session(const cmdline::ParsedArgs& args, std::string_view file, const std::function<query::Outcome<Json>(query::View&, Clock::time_point)>& ask,
                   const std::function<void(const Json&)>& printText, const std::function<int(const Json&)>& exitStatus = {}) {
    if (!args.value("log-level")) base::log::set_level(base::log::Level::warning);
    apply_log_level(args);
    const std::string format { args.value("format").value_or("json") };
    // sarif and markdown are what review renders its result as: JSON, and a document of text.
    if (format != "json" && format != "text" && format != "sarif" && format != "markdown" && format != "lsp") {
        std::println(std::cerr, "unknown format {}; use json or text (review: also sarif, markdown or lsp)", format);
        return EXIT_FAILED;
    }
    const orchestrator::KernelOptions options { session_options(args), root_of(args, file) };
    auto kernel = orchestrator::Kernel::start(options);
    query::View view { *kernel };
    const auto deadline = Clock::now() + seconds_option(args, "timeout", std::chrono::seconds { 180 });
    auto result = ask(view, deadline);
    kernel->shut_down();
    if (!result) {
        const Json error { { "error", query::to_json(result.error()) } };
        if (format == "json") std::println("{}", error.dump(2));
        else std::println(std::cerr, "{}: {}", result.error().code, result.error().message);
        return result.error().code == "not-found" || result.error().code == "ambiguous" ? EXIT_NOTHING : EXIT_FAILED;
    }
    if (format == "text") {
        printText(*result);
        return exitStatus ? exitStatus(*result) : 0;
    }
    const std::string document { format == "markdown" ? result->value("markdown", std::string {}) : result->dump(2) + "\n" };
    if (auto output = args.value("output")) {
        if (auto written = platform::fs::write_file(absolute(*output), document); !written) {
            std::println(std::cerr, "cannot write {}: {}", *output, written.error().message);
            return EXIT_FAILED;
        }
    } else {
        std::print("{}", document);
    }
    return exitStatus ? exitStatus(*result) : 0;
}

query::SymbolTarget symbol_target(const cmdline::ParsedArgs& args) {
    query::SymbolTarget target;
    target.name = args.value("name").value_or("");
    target.id = args.value("id").value_or("");
    target.kind = args.value("kind").value_or("");
    target.module = args.value("module").value_or("");
    if (auto at = args.value("at")) {
        if (auto position = parse_position(*at)) {
            target.file = absolute(position->file);
            target.line = position->line;
            target.column = position->column;
        }
    }
    return target;
}

void add_session_options(cmdline::App& command) {
    (void)command.option("root").takes_value().help("Workspace root (default: the nearest build description, else the current directory)");
    (void)command.option("timeout").takes_value().help("Seconds to wait for the engines (default 180)");
    (void)command.option("format").takes_value().help("json (default) | text");
}

void add_target_options(cmdline::App& command) {
    (void)command.arg("name").help("A symbol name, qualified or not");
    (void)command.option("at").takes_value().help("FILE:LINE:COLUMN of the symbol instead of a name (lines and columns from 1)");
    (void)command.option("id").takes_value().help("The symbol's id from an earlier result");
    (void)command.option("max").takes_value().help("At most this many results");
}

} // namespace

cmdline::App query_command(bool& handled, int& status) {
    cmdline::App command { "query" };
    (void)command.description("Answer a question about the code: symbols, references, calls, outlines, modules");
    // cmdline runs the action of one level of subcommands; the query's own action runs the next.
    auto actions = std::make_shared<std::map<std::string, std::function<void(const cmdline::ParsedArgs&)>, std::less<>>>();
    (void)command.action([actions, &handled, &status](const cmdline::ParsedArgs& args) {
        const auto sub = args.subcommand();
        const auto found = actions->find(args.subcommand_name());
        if (!sub || found == actions->end()) {
            handled = true;
            std::println(std::cerr, "query: name what to query: symbol, refs, calls, outline or module (mcppls query --help)");
            status = EXIT_FAILED;
            return;
        }
        found->second(sub->get());
    });
    auto on = [&](cmdline::App& sub, std::string name, std::function<void(const cmdline::ParsedArgs&)> action) {
        (void)sub.action(action);
        (*actions)[std::move(name)] = std::move(action);
    };

    cmdline::App symbol { "symbol" };
    (void)symbol.description("Find a symbol by name, id or position, with its declaration, definition, signature and module");
    add_target_options(symbol);
    (void)symbol.option("kind").takes_value().help("Only symbols of this kind (function, class, namespace, ...)");
    (void)symbol.option("module").takes_value().help("Only symbols declared in this module");
    add_session_options(symbol);
    on(symbol, "symbol", [&](const cmdline::ParsedArgs& args) {
        handled = true;
        const auto target = symbol_target(args);
        status = run_in_session(args, target.file, [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            auto found = ai::context::locate_symbols(view, target, query::Limit { max_results(args, 20) }, true, deadline);
            if (!found) return std::unexpected { found.error() };
            if (found->symbols.empty()) return std::unexpected { query::not_found(std::format("no symbol named {}", target.name)) };
            return query::to_json(*found);
        }, print_text_symbols);
    });
    (void)command.subcommand(std::move(symbol));

    cmdline::App refs { "refs" };
    (void)refs.description("List the references to a symbol, grouped by module and file");
    add_target_options(refs);
    (void)refs.option("no-declaration").help("Leave out the declaration itself");
    add_session_options(refs);
    on(refs, "refs", [&](const cmdline::ParsedArgs& args) {
        handled = true;
        const auto target = symbol_target(args);
        status = run_in_session(args, target.file, [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            auto found = query::find_references(view, target, !args.is_flag_set("no-declaration"), query::Limit { max_results(args, 200) }, deadline);
            if (!found) return std::unexpected { found.error() };
            return query::to_json(*found);
        }, print_text_references);
    });
    (void)command.subcommand(std::move(refs));

    cmdline::App calls { "calls" };
    (void)calls.description("List the callers of a function, or with --callees what it calls");
    add_target_options(calls);
    (void)calls.option("callees").help("What the function calls, instead of its callers");
    add_session_options(calls);
    on(calls, "calls", [&](const cmdline::ParsedArgs& args) {
        handled = true;
        const auto target = symbol_target(args);
        status = run_in_session(args, target.file, [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            auto found = query::find_calls(view, target, args.is_flag_set("callees") ? query::CallDirection::outgoing : query::CallDirection::incoming,
                                           query::Limit { max_results(args, 100) }, deadline);
            if (!found) return std::unexpected { found.error() };
            return query::to_json(*found);
        }, print_text_calls);
    });
    (void)command.subcommand(std::move(calls));

    cmdline::App outline { "outline" };
    (void)outline.description("A compact outline of a file, with its module declaration");
    (void)outline.arg("file").required();
    add_session_options(outline);
    on(outline, "outline", [&](const cmdline::ParsedArgs& args) {
        handled = true;
        const std::string file { absolute(args.value("file").value_or("")) };
        status = run_in_session(args, file, [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            auto found = query::outline_file(view, file, deadline);
            if (!found) return std::unexpected { found.error() };
            return query::to_json(*found);
        }, print_text_outline);
    });
    (void)command.subcommand(std::move(outline));

    cmdline::App module { "module" };
    (void)module.description("A module's units, partitions, imports and importers; with --graph the module graph");
    (void)module.arg("name").help("The module (a partition is described as part of its module)");
    (void)module.option("file").takes_value().help("The module of this file instead of a name");
    (void)module.option("graph").help("The module graph, of modules whose names contain the name when one is given");
    (void)module.option("interface").help("Include the exported declarations of the module and the partitions it re-exports");
    (void)module.option("max-tokens").takes_value().help("Budget of the interface (default 2000)");
    add_session_options(module);
    on(module, "module", [&](const cmdline::ParsedArgs& args) {
        handled = true;
        const std::string file { args.value("file") ? absolute(*args.value("file")) : std::string {} };
        status = run_in_session(args, file, [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            (void)view.settle(deadline);
            if (args.is_flag_set("graph")) return query::to_json(query::module_graph(view, args.value("name").value_or(""), query::Limit { max_results(args, 500) }));
            auto found = query::describe_module(view, args.value("name").value_or(""), file);
            if (!found) return std::unexpected { found.error() };
            Json value = query::to_json(*found);
            if (args.is_flag_set("interface") && !found->external) {
                std::size_t tokens { 2000 };
                if (auto text = args.value("max-tokens")) {
                    try {
                        tokens = static_cast<std::size_t>(std::max(100, std::stoi(*text)));
                    } catch (...) {
                    }
                }
                if (auto interface = ai::context::module_interface(view, found->name, tokens * 4)) {
                    Json summary = ai::context::to_json(*interface);
                    summary.erase("snapshot");
                    value["interface"] = std::move(summary);
                }
            }
            return value;
        }, print_text_module);
    });
    (void)command.subcommand(std::move(module));

    cmdline::App context { "context" };
    (void)context.description("How a file is built and read: sets and role, compiler, standard library, language standard, macros");
    (void)context.arg("file").required();
    add_session_options(context);
    on(context, "context", [&](const cmdline::ParsedArgs& args) {
        handled = true;
        const std::string file { absolute(args.value("file").value_or("")) };
        status = run_in_session(args, file, [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            auto found = ai::context::build_context(view, file, deadline);
            if (!found) return std::unexpected { found.error() };
            return ai::context::to_json(*found);
        }, [](const Json& result) { std::println("{}", result.dump(2)); });
    });
    (void)command.subcommand(std::move(context));
    return command;
}

cmdline::App verify_command(bool& handled, int& status) {
    cmdline::App command { "verify" };
    (void)command.description("Check an edit: changed files and the units that import them, or a snippet placed in a file without writing it");
    (void)command.arg("file").help("Files that changed");
    (void)command.option("changed").help("The working tree's changes, from git");
    (void)command.option("base").takes_value().help("The changes since this git revision");
    (void)command.option("budget").takes_value().help("Files checked at most (default 32)");
    (void)command.option("snippet").takes_value().help("FILE:LINE where the code of --code or --code-file goes");
    (void)command.option("code").takes_value().help("The snippet's code");
    (void)command.option("code-file").takes_value().help("A file holding the snippet's code");
    (void)command.option("replace-lines").takes_value().help("Lines of the file the snippet replaces (default 0)");
    (void)command.option("toolchains").takes_value().help("Build the project with these mcpp toolchains, comma-separated, and compare their errors");
    add_session_options(command);
    (void)command.action([&](const cmdline::ParsedArgs& args) {
        handled = true;
        auto verdict_status = [](const Json& result) {
            const std::string verdict { result.value("verdict", std::string {}) };
            return verdict == "pass" ? 0 : verdict == "errors" ? 1 : EXIT_FAILED;
        };
        auto print = [](const Json& result) {
            std::println("{}", result.value("verdict", std::string {}));
            for (const std::string_view key : { "inSnippet", "introduced" }) {
                for (const auto& diagnostic : result.value(std::string { key }, Json::array())) {
                    std::println("{}: {}: {}", location_text(diagnostic["location"]), diagnostic.value("severity", std::string {}), diagnostic.value("message", std::string {}));
                }
            }
            for (const auto& file : result.value("checked", Json::array())) {
                for (const auto& diagnostic : file.value("diagnostics", Json::array())) {
                    std::println("{}: {}: {}", location_text(diagnostic["location"]), diagnostic.value("severity", std::string {}), diagnostic.value("message", std::string {}));
                }
            }
        };
        if (auto at = args.value("snippet")) {
            const auto position = parse_position(*at);
            std::string code { args.value("code").value_or("") };
            if (auto codeFile = args.value("code-file")) code = platform::fs::read_file(absolute(*codeFile)).value_or("");
            if (!position) {
                std::println(std::cerr, "verify: --snippet takes FILE:LINE");
                status = EXIT_FAILED;
                return;
            }
            const std::string file { absolute(position->file) };
            int replace { 0 };
            try {
                replace = std::stoi(args.value("replace-lines").value_or("0"));
            } catch (...) {
            }
            status = run_in_session(args, file, [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
                auto verified = ai::verify::verify_snippet(view, ai::verify::SnippetOptions { file, position->line, replace, code }, deadline);
                if (!verified) return std::unexpected { verified.error() };
                return ai::verify::to_json(*verified);
            }, print, verdict_status);
            return;
        }
        if (auto list = args.value("toolchains")) {
            std::vector<std::string> toolchains;
            for (auto toolchain : base::split(*list, ',')) {
                if (!base::trim(toolchain).empty()) toolchains.emplace_back(base::trim(toolchain));
            }
            status = run_in_session(args, {}, [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
                auto compared = ai::verify::compare_toolchains(view, toolchains, deadline);
                if (!compared) return std::unexpected { compared.error() };
                Json value = ai::verify::to_json(*compared);
                const bool allBuilt { std::ranges::all_of(compared->builds, [](const auto& build) { return build.built; }) };
                const bool allRan { std::ranges::all_of(compared->builds, [](const auto& build) { return build.ran; }) };
                value["verdict"] = allBuilt ? "pass" : allRan ? "errors" : "incomplete";
                return value;
            }, [](const Json& result) {
                for (const auto& divergence : result.value("divergences", Json::array())) {
                    std::println("{}: {} only: {}", location_text(divergence["diagnostic"]["location"]), divergence.value("toolchain", std::string {}),
                                 divergence["diagnostic"].value("message", std::string {}));
                }
                std::println("{}", result.value("verdict", std::string {}));
            }, verdict_status);
            return;
        }
        ai::verify::ChangeOptions options;
        for (const auto& file : args.positionals) options.files.push_back(absolute(file));
        options.workingTree = args.is_flag_set("changed");
        options.base = args.value("base").value_or("");
        options.budget = static_cast<std::size_t>(std::max(1, static_cast<int>(max_results(args, 32))));
        if (auto budget = args.value("budget")) {
            try {
                options.budget = static_cast<std::size_t>(std::max(1, std::stoi(*budget)));
            } catch (...) {
            }
        }
        status = run_in_session(args, options.files.empty() ? std::string {} : options.files.front(), [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            auto verified = ai::verify::verify_changes(view, options, deadline);
            if (!verified) return std::unexpected { verified.error() };
            return ai::verify::to_json(*verified);
        }, print, verdict_status);
    });
    return command;
}

namespace {

ai::review::ReviewRequest review_request_of(const cmdline::ParsedArgs& args) {
    ai::review::ReviewRequest request;
    request.changes.base = args.value("base").value_or("HEAD");
    for (const auto& file : args.positionals) request.changes.files.push_back(absolute(file));
    if (auto budget = args.value("budget")) {
        try {
            request.budget = static_cast<std::size_t>(std::max(1, std::stoi(*budget)));
        } catch (...) {
        }
    }
    if (auto toolchains = args.value("toolchains")) {
        for (auto toolchain : base::split(*toolchains, ',')) {
            if (!base::trim(toolchain).empty()) request.toolchains.emplace_back(base::trim(toolchain));
        }
    }
    return request;
}

void add_change_options(cmdline::App& command) {
    (void)command.arg("file").help("Only these files, against the base");
    (void)command.option("base").takes_value().help("The git revision the change is against (default HEAD)");
    (void)command.option("budget").takes_value().help("Units searched and built at most (default 32)");
    (void)command.option("toolchains").takes_value().help("mcpp toolchains to build the project with, comma-separated, e.g. gcc@16.1.0,llvm@22.1.8");
}

void print_text_impact(const Json& result) {
    for (const auto& diff : result.value("diffs", Json::array())) {
        if (!diff.value("interfaceChanged", false)) continue;
        std::println("{}: interface changed", diff.value("file", std::string {}));
        for (const auto& change : diff.value("exports", Json::array())) {
            std::println("  {} {} {}", change.value("change", std::string {}), change.value("kind", std::string {}), change.value("qualifiedName", std::string {}));
        }
    }
    const Json impact = result.value("impact", Json::object());
    for (const auto& name : impact.value("names", Json::array())) {
        std::println("{}: {} use(s)", name.value("qualifiedName", std::string {}), name.value("uses", Json::array()).size());
        for (const auto& use : name.value("uses", Json::array())) std::println("  {}  {}", location_text(use), base::trim(use.value("text", std::string {})));
    }
    for (const auto& test : impact.value("tests", Json::array())) {
        std::println("test set {}{}", test.value("set", std::string {}), test.value("changed", false) ? " (changed)" : "");
    }
}

} // namespace

cmdline::App impact_command(bool& handled, int& status) {
    cmdline::App command { "impact" };
    (void)command.description("What a change does to module interfaces and what it can break");
    add_change_options(command);
    add_session_options(command);
    (void)command.action([&](const cmdline::ParsedArgs& args) {
        handled = true;
        auto request = review_request_of(args);
        request.build = false;
        status = run_in_session(args, request.changes.files.empty() ? std::string {} : request.changes.files.front(),
                                [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            auto analyzed = ai::review::analyze_change(view, request, deadline);
            if (!analyzed) return std::unexpected { analyzed.error() };
            return ai::review::impact_json(*analyzed);
        }, print_text_impact);
    });
    return command;
}

cmdline::App review_command(bool& handled, int& status) {
    cmdline::App command { "review" };
    (void)command.description("Review a change: findings of the rules, with their evidence; SARIF for code scanning");
    add_change_options(command);
    add_session_options(command);
    (void)command.option("output").takes_value().help("Write the report to this file instead of standard output");
    add_model_options(command, "model", "A model's judgement on top of the rules: agent (the context and instructions, for you) or gateway (default none)");
    (void)command.option("explain-context").help("With --model, list what would be sent to the model, and send nothing");
    (void)command.action([&](const cmdline::ParsedArgs& args) {
        handled = true;
        const auto request = review_request_of(args);
        const std::string format { args.value("format").value_or("json") };
        const auto modelSettings = model_settings(args, "model");
        if (!modelSettings || modelSettings->source == ai::model::SourceKind::mcp_sampling || modelSettings->source == ai::model::SourceKind::client) {
            std::println(std::cerr, "review: --model is none, agent or gateway");
            status = EXIT_FAILED;
            return;
        }
        std::optional<ai::review::ReviewResult> kept;
        const int code = run_in_session(args, request.changes.files.empty() ? std::string {} : request.changes.files.front(),
                                        [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            auto reviewed = ai::review::review_change(view, request, deadline);
            if (!reviewed) return std::unexpected { reviewed.error() };
            Json value = ai::review::review_json(*reviewed);
            if (modelSettings->source != ai::model::SourceKind::none) {
                std::unique_ptr<ai::model::ModelClient> client;
                if (modelSettings->source == ai::model::SourceKind::gateway) {
                    ai::model::GatewayOptions gateway;
                    gateway.executable = modelSettings->gatewayExecutable.empty() ? platform::env::find_executable("mcppls-model").value_or("") : modelSettings->gatewayExecutable;
                    if (!modelSettings->model.empty()) gateway.arguments = { "--model", modelSettings->model };
                    gateway.workDirectory = view.root();
                    if (!gateway.executable.empty()) client = std::make_unique<ai::model::GatewayClient>(std::move(gateway));
                }
                const ai::review::JudgementOptions options { *modelSettings, base::join_path(view.kernel().workspace().cache_directory(), "model"),
                                                              args.is_flag_set("explain-context"), [&view, deadline](const Json& fix) {
                                                                  const auto verified = ai::verify::verify_fix(view, fix, deadline);
                                                                  return verified && verified->passes;
                                                              } };
                const auto judgement = ai::review::judge(*reviewed, options, client.get());
                for (const auto& finding : judgement.findings) {
                    value["findings"].push_back(spec::to_json(finding));
                    reviewed->findings.push_back(finding);
                }
                value["model"] = ai::review::to_json(judgement);
            }
            if (format == "sarif") {
                value = ai::review::to_sarif(reviewed->findings, view.root(), request.changes.base);
            } else if (format == "markdown") {
                value = Json { { "markdown", ai::review::to_markdown(reviewed->findings, request.changes.base, value) } };
            } else if (format == "lsp") {
                // What a language server publishes for an editor's review command: LSP diagnostics by document URI.
                Json byUri = Json::object();
                const auto uri_of = [&](std::string_view file) { return base::path_to_uri(base::is_absolute_path(file) ? std::string { file } : base::join_path(view.root(), file)); };
                for (const auto& finding : reviewed->findings) {
                    const std::string uri { uri_of(finding.location.file) };
                    if (!byUri.contains(uri)) byUri[uri] = Json::array();
                    byUri[uri].push_back(ai::review::to_lsp_diagnostic(finding, uri_of));
                }
                value = Json { { "base", request.changes.base }, { "diagnostics", std::move(byUri) }, { "counts", value["counts"] }, { "complete", value["complete"] } };
            }
            kept = std::move(*reviewed);
            return value;
        }, [](const Json& result) {
            for (const auto& finding : result.value("findings", Json::array())) {
                std::println("{}: {}: {} [{}]", location_text(finding["location"]), finding.value("severity", std::string {}), finding.value("message", std::string {}),
                             finding.value("rule", std::string {}));
                for (const auto& evidence : finding.value("evidence", Json::array())) {
                    std::println("  {} {} {}", evidence.value("id", std::string {}), evidence.value("kind", std::string {}), location_text(evidence["location"]));
                }
            }
        }, [](const Json&) { return 0; });
        if (code != 0 || !kept) {
            status = code;
            return;
        }
        status = std::ranges::any_of(kept->findings, [](const spec::Finding& finding) { return finding.severity == spec::Severity::error; }) ? 1 : 0;
    });
    return command;
}

cmdline::App diagnostics_command(bool& handled, int& status) {
    cmdline::App command { "diagnostics" };
    (void)command.description("The diagnostics of files, computed for their current content");
    (void)command.arg("file").required();
    (void)command.option("no-fresh").help("Do not wait for the core engine's diagnostics");
    add_session_options(command);
    (void)command.action([&](const cmdline::ParsedArgs& args) {
        handled = true;
        std::vector<std::string> files;
        for (const auto& file : args.positionals) files.push_back(absolute(file));
        status = run_in_session(args, files.empty() ? std::string {} : files.front(), [&](query::View& view, Clock::time_point deadline) -> query::Outcome<Json> {
            auto found = query::file_diagnostics(view, files, !args.is_flag_set("no-fresh"), deadline);
            if (!found) return std::unexpected { found.error() };
            return query::to_json(*found);
        }, print_text_diagnostics, [](const Json& result) {
            const Json counts = result.value("counts", Json::object());
            bool incomplete { false };
            for (const auto& file : result.value("files", Json::array())) incomplete = incomplete || !file.value("complete", true);
            if (counts.value("error", 0) > 0) return 1;
            return incomplete ? EXIT_FAILED : 0;
        });
    });
    return command;
}

} // namespace mcppls::cli
