module mcppls.ai.review.rules;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.spec.database;
import mcppls.spec.query;
import mcppls.project.scan;
import mcppls.engine.native.index;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.query.view;
import mcppls.ai.query.files;
import mcppls.ai.query.modules;
import mcppls.ai.review.changes;
import mcppls.ai.review.semantic;
import mcppls.ai.review.impact;

namespace mcppls::ai::review {

namespace {

constexpr std::array<Rule, 7> RULES {
    Rule { "module/export-removed-in-use", "Removed export still in use", spec::Severity::error,
           "An exported declaration, or the module itself, went away while units that import it still use it." },
    Rule { "module/export-signature-changed", "Exported signature changed", spec::Severity::warning,
           "An exported declaration changed; the units that use it are listed to be checked." },
    Rule { "module/partition-misuse", "Partition misuse", spec::Severity::error,
           "An implementation partition is exported, or a partition is imported from outside its module." },
    Rule { "module/import-unresolved", "Unresolved import", spec::Severity::error,
           "The change imports a module that no unit of the workspace and no standard library provides, or that several provide." },
    Rule { "build/diagnostic-introduced", "Diagnostic in changed code", spec::Severity::error,
           "The compiler reports an error or a warning on a line the change added or altered." },
    Rule { "build/toolchain-divergence", "Fails with one toolchain only", spec::Severity::error,
           "One toolchain the project builds with rejects code the change touches, while another builds the project." },
    Rule { "test/exported-change-untested", "Interface change without a test change", spec::Severity::information,
           "A module's interface changed and no test that uses the module changed with it." },
};

constexpr std::size_t MAX_EVIDENCE { 20 };

struct Builder {
    std::vector<spec::Finding> findings;

    spec::Finding& add(std::string_view rule, spec::Severity severity, std::string message, spec::Location location) {
        spec::Finding finding;
        finding.rule = std::string { rule };
        finding.severity = severity;
        finding.message = std::move(message);
        finding.location = std::move(location);
        findings.push_back(std::move(finding));
        return findings.back();
    }

    static void evidence(spec::Finding& finding, std::string kind, spec::Location location, std::string detail = {}) {
        if (finding.evidence.size() >= MAX_EVIDENCE) return;
        finding.evidence.push_back(spec::Evidence { std::format("E{}", finding.evidence.size() + 1), std::move(kind), std::move(location), std::move(detail) });
    }
};

std::size_t files_in(std::span<const spec::Location> uses) {
    std::set<std::string> files;
    for (const auto& use : uses) files.insert(use.file);
    return files.size();
}

const NameUses* uses_of(const Impact& impact, std::string_view qualifiedName) {
    const auto found = std::ranges::find_if(impact.names, [&](const NameUses& uses) { return uses.qualifiedName == qualifiedName; });
    return found == impact.names.end() ? nullptr : &*found;
}

// An error the importer's compiler reports on the line of a use: the evidence that the use is broken.
void add_diagnostic_at_use(spec::Finding& finding, const RuleInput& input, const spec::Location& use) {
    const auto diagnostics = input.diagnostics.find(use.file);
    if (diagnostics == input.diagnostics.end()) return;
    for (const auto& diagnostic : diagnostics->second) {
        if (diagnostic.severity != spec::Severity::error || diagnostic.location.line != use.line) continue;
        Builder::evidence(finding, "diagnostic", diagnostic.location, diagnostic.message);
        return;
    }
}

} // namespace

std::span<const Rule> rules() { return RULES; }

const Rule* find_rule(std::string_view id) {
    const auto found = std::ranges::find(RULES, id, &Rule::id);
    return found == RULES.end() ? nullptr : &*found;
}

std::vector<spec::Finding> run_rules(query::View& view, const RuleInput& input) {
    Builder builder;
    const auto& moduleIndex = view.kernel().workspace().module_index();

    for (std::size_t i { 0 }; i < input.diffs.size() && i < input.changes.files.size(); ++i) {
        const UnitDiff& diff { input.diffs[i] };
        const FileChange& file { input.changes.files[i] };

        // module/export-removed-in-use: an export that went away, still named by its users.
        for (const auto& change : diff.exports) {
            if (change.kind != ExportChangeKind::removed || !change.baseLocation) continue;
            const NameUses* uses { uses_of(input.impact, change.qualifiedName) };
            if (uses == nullptr || uses->uses.empty()) continue;
            auto& finding = builder.add("module/export-removed-in-use", spec::Severity::error,
                                        std::format("{} no longer exports {}, which {} use{} in {} file{} still name", diff.baseModule, change.qualifiedName,
                                                    uses->uses.size(), uses->uses.size() == 1 ? "" : "s", files_in(uses->uses), files_in(uses->uses) == 1 ? "" : "s"),
                                        *change.baseLocation);
            Builder::evidence(finding, "diff", *change.baseLocation, "-" + change.before);
            for (const auto& use : uses->uses) {
                Builder::evidence(finding, "reference", use);
                add_diagnostic_at_use(finding, input, use);
            }
        }
        // A module that is gone, or renamed, while units still import it by its old name.
        if (!diff.baseModule.empty() && diff.baseModule != diff.headModule && diff.baseRole != "module-implementation" && !diff.baseRole.empty()) {
            std::vector<std::pair<std::string, spec::Location>> importers;
            for (const auto& path : query::importers_of(view, diff.baseModule)) {
                const auto* scan = moduleIndex.scan_of(path);
                if (scan == nullptr) continue;
                const std::string text { view.text_of(path) };
                for (const auto& import : scan->imports) {
                    if (import.isHeaderUnit || project::imported_name(*scan, import) != diff.baseModule) continue;
                    importers.emplace_back(path, spec::location_in(text, view.display(path), import.nameRange.start, import.nameRange.end));
                }
            }
            if (!importers.empty()) {
                const spec::Location at { spec::location_in(file.base.value_or(std::string {}), diff.file, base::Position {}, std::nullopt) };
                auto& finding = builder.add("module/export-removed-in-use", spec::Severity::error,
                                            diff.headModule.empty()
                                                ? std::format("{} no longer provides module {}, which {} unit{} still import", diff.file, diff.baseModule, importers.size(),
                                                              importers.size() == 1 ? "" : "s")
                                                : std::format("module {} is now {}, and {} unit{} still import {}", diff.baseModule, diff.headModule, importers.size(),
                                                              importers.size() == 1 ? "" : "s", diff.baseModule),
                                            at);
                for (const auto& [path, location] : importers) Builder::evidence(finding, "import", location);
            }
        }

        // module/export-signature-changed: users of a declaration that changed.
        for (const auto& change : diff.exports) {
            if (change.kind != ExportChangeKind::changed || !change.headLocation) continue;
            const NameUses* uses { uses_of(input.impact, change.qualifiedName) };
            if (uses == nullptr || uses->uses.empty()) continue;
            auto& finding = builder.add("module/export-signature-changed", spec::Severity::warning,
                                        std::format("{} changed from `{}` to `{}`; {} use{} in {} file{} depend on it", change.qualifiedName, change.before, change.after,
                                                    uses->uses.size(), uses->uses.size() == 1 ? "" : "s", files_in(uses->uses), files_in(uses->uses) == 1 ? "" : "s"),
                                        *change.headLocation);
            if (change.baseLocation) Builder::evidence(finding, "diff", *change.baseLocation, "-" + change.before);
            Builder::evidence(finding, "diff", *change.headLocation, "+" + change.after);
            for (const auto& use : uses->uses) {
                Builder::evidence(finding, "reference", use);
                add_diagnostic_at_use(finding, input, use);
            }
        }

        if (!file.head) continue;
        const project::ScanResult headScan { project::scan_source(*file.head) };
        for (const auto& import : headScan.imports) {
            if (import.isHeaderUnit || !file.changed_head_line(import.nameRange.start.line + 1)) continue;
            const std::string name { project::imported_name(headScan, import) };
            const spec::Location at { spec::location_in(*file.head, diff.file, import.nameRange.start, import.nameRange.end) };

            // module/partition-misuse
            if (!import.module.empty() && !import.partition.empty()) {
                auto& finding = builder.add("module/partition-misuse", spec::Severity::error,
                                            std::format("{} imports partition {} by its full name; a partition is imported only inside its own module, as `import :{};`",
                                                        diff.file, name, import.partition),
                                            at);
                Builder::evidence(finding, "import", at);
                continue;
            }
            const auto providers = moduleIndex.providers(name);
            if (import.isExported && import.module.empty() && !import.partition.empty()) {
                const auto implementation = std::ranges::find_if(providers, [](const auto& unit) { return unit.role == spec::Role::module_partition_implementation; });
                if (implementation != providers.end()) {
                    auto& finding = builder.add("module/partition-misuse", spec::Severity::error,
                                                std::format("{} re-exports {}, an implementation partition; only an interface partition can be exported", diff.file, name),
                                                at);
                    Builder::evidence(finding, "import", at);
                    const std::string text { view.text_of(implementation->path) };
                    Builder::evidence(finding, "module-graph",
                                      spec::location_in(text, view.display(implementation->path), implementation->declaration.start, implementation->declaration.end),
                                      std::format("{} is declared `module {};`, without export", name, name));
                }
            }

            // module/import-unresolved
            if (!providers.empty() && providers.size() == 1) continue;
            if (providers.empty() && moduleIndex.external(name) != nullptr) continue;
            auto& finding = builder.add("module/import-unresolved", spec::Severity::error,
                                        providers.empty() ? std::format("{} imports {}, which no unit of the workspace or its toolchain provides", diff.file, name)
                                                          : std::format("{} imports {}, which {} units provide", diff.file, name, providers.size()),
                                        at);
            Builder::evidence(finding, "import", at);
            for (const auto& provider : providers) {
                const std::string text { view.text_of(provider.path) };
                Builder::evidence(finding, "module-graph", spec::location_in(text, view.display(provider.path), provider.declaration.start, provider.declaration.end));
            }
        }

        // build/diagnostic-introduced
        if (const auto diagnostics = input.diagnostics.find(diff.file); diagnostics != input.diagnostics.end()) {
            for (const auto& diagnostic : diagnostics->second) {
                if (diagnostic.severity != spec::Severity::error && diagnostic.severity != spec::Severity::warning) continue;
                if (diagnostic.code == "unresolved-module" || !file.changed_head_line(diagnostic.location.line)) continue;
                auto& finding = builder.add("build/diagnostic-introduced", diagnostic.severity, diagnostic.message, diagnostic.location);
                Builder::evidence(finding, "diagnostic", diagnostic.location, diagnostic.source.empty() ? diagnostic.code : diagnostic.source);
            }
        }
    }

    // build/toolchain-divergence: in the changed lines, or in a unit that can use a changed interface.
    if (input.toolchains != nullptr) {
        std::set<std::string> involved { input.impact.files.begin(), input.impact.files.end() };
        for (const auto& divergence : input.toolchains->divergences) {
            const auto& at = divergence.diagnostic.location;
            const auto changed = std::ranges::find_if(input.diffs, [&](const UnitDiff& diff) { return diff.file == at.file; });
            bool relevant { involved.contains(at.file) };
            if (changed != input.diffs.end()) {
                const auto index = static_cast<std::size_t>(changed - input.diffs.begin());
                relevant = relevant || (index < input.changes.files.size() && input.changes.files[index].changed_head_line(at.line));
            }
            if (!relevant) continue;
            std::string others;
            for (const auto& other : divergence.others) others += (others.empty() ? "" : ", ") + other;
            auto& finding = builder.add("build/toolchain-divergence", spec::Severity::error,
                                        std::format("{} rejects this, while {} build{} the project: {}", divergence.toolchain, others,
                                                    divergence.others.size() == 1 ? "s" : "", divergence.diagnostic.message),
                                        at);
            Builder::evidence(finding, "diagnostic", at, std::format("{}: {}", divergence.toolchain, divergence.diagnostic.message));
        }
    }

    // test/exported-change-untested: per module whose exports changed.
    std::map<std::string, const ExportChange*> changedModules;
    for (const auto& diff : input.diffs) {
        if (diff.exports.empty()) continue;
        const std::string name { diff.headModule.empty() ? diff.baseModule : diff.headModule };
        const std::string primary { name.substr(0, name.find(':')) };
        if (!primary.empty() && !changedModules.contains(primary)) changedModules.emplace(primary, &diff.exports.front());
    }
    const bool testChanged { std::ranges::any_of(input.impact.tests, [](const TestCoverage& test) { return test.changed; }) };
    for (const auto& [primary, change] : changedModules) {
        if (testChanged) break;
        const auto& at = change->headLocation ? change->headLocation : change->baseLocation;
        if (!at) continue;
        std::vector<std::string> covering;
        for (const auto& test : input.impact.tests) {
            for (const auto& testFile : test.files) covering.push_back(testFile);
        }
        auto& finding = builder.add("test/exported-change-untested", spec::Severity::information,
                                    covering.empty() ? std::format("the interface of {} changed and no test uses it", primary)
                                                     : std::format("the interface of {} changed and none of the {} test unit{} that use it changed", primary,
                                                                   covering.size(), covering.size() == 1 ? "" : "s"),
                                    *at);
        Builder::evidence(finding, "diff", *at, std::format("{}{}", change->kind == ExportChangeKind::removed ? "-" : "+", change->kind == ExportChangeKind::removed ? change->before : change->after));
        for (const auto& testFile : covering) {
            const std::string path { view.path_of(testFile) };
            Builder::evidence(finding, "test", spec::location_in(view.text_of(path), testFile, base::Position {}, std::nullopt));
        }
    }

    auto& findings = builder.findings;
    // A compiler diagnostic on the line of another rule's finding is that finding's evidence, not a finding of its own.
    for (auto diagnostic = findings.begin(); diagnostic != findings.end();) {
        if (diagnostic->rule != "build/diagnostic-introduced") {
            ++diagnostic;
            continue;
        }
        const auto owner = std::ranges::find_if(findings, [&](const spec::Finding& other) {
            return other.rule != "build/diagnostic-introduced" && other.location.file == diagnostic->location.file && other.location.line == diagnostic->location.line;
        });
        if (owner == findings.end()) {
            ++diagnostic;
            continue;
        }
        Builder::evidence(*owner, "diagnostic", diagnostic->location, diagnostic->message);
        diagnostic = findings.erase(diagnostic);
    }
    std::ranges::sort(findings, {}, [](const spec::Finding& f) { return std::tuple { f.location.file, f.location.line, f.location.column, f.rule }; });
    for (std::size_t i { 0 }; i < findings.size(); ++i) {
        findings[i].id = std::format("F{}", i + 1);
        findings[i].fingerprint = spec::fingerprint_of(findings[i]);
    }
    return std::move(findings);
}

} // namespace mcppls::ai::review
