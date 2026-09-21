module mcppls.ai.review.report;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.base.version;
import mcppls.spec.query;
import mcppls.ai.review.rules;

namespace mcppls::ai::review {

namespace {

using Json = nlohmann::json;

std::string_view sarif_level(spec::Severity severity) {
    switch (severity) {
    case spec::Severity::error: return "error";
    case spec::Severity::warning: return "warning";
    case spec::Severity::information:
    case spec::Severity::hint: return "note";
    }
    return "warning";
}

Json region_of(const spec::Location& location) {
    Json region { { "startLine", std::max(location.line, 1) }, { "startColumn", std::max(location.column, 1) } };
    if (location.endLine) region["endLine"] = *location.endLine;
    if (location.endColumn) region["endColumn"] = *location.endColumn;
    if (!location.text.empty()) region["snippet"] = Json { { "text", location.text } };
    return region;
}

Json physical_location(const spec::Location& location) {
    std::string uri { location.file };
    std::ranges::replace(uri, '\\', '/');
    Json artifact { { "uri", base::is_absolute_path(location.file) ? base::path_to_uri(location.file) : base::percent_encode_path(uri) } };
    if (!base::is_absolute_path(location.file)) artifact["uriBaseId"] = "%SRCROOT%";
    return Json { { "artifactLocation", std::move(artifact) }, { "region", region_of(location) } };
}

Json lsp_range(const spec::Location& location) {
    const auto start = spec::lsp_position(location.text, 1, location.column);
    int endCharacter { start.character };
    int endLine { location.line - 1 };
    if (location.endLine && location.endColumn) {
        endLine = *location.endLine - 1;
        // The end's own line text is known only when the range stays on one line.
        endCharacter = *location.endLine == location.line ? spec::lsp_position(location.text, 1, *location.endColumn).character
                                                          : *location.endColumn - 1;
    } else {
        endCharacter = static_cast<int>(base::utf16_length(location.text));
    }
    return Json { { "start", Json { { "line", location.line - 1 }, { "character", start.character } } },
                  { "end", Json { { "line", endLine }, { "character", endCharacter } } } };
}

} // namespace

Json to_sarif(std::span<const spec::Finding> findings, std::string_view root, std::string_view base) {
    Json rulesJson = Json::array();
    std::map<std::string, std::size_t> ruleIndex;
    auto index_of = [&](std::string_view id) {
        if (const auto found = ruleIndex.find(std::string { id }); found != ruleIndex.end()) return found->second;
        Json rule { { "id", std::string { id } } };
        if (const auto* known = find_rule(id)) {
            rule["name"] = std::string { known->title };
            rule["shortDescription"] = Json { { "text", std::string { known->title } } };
            rule["fullDescription"] = Json { { "text", std::string { known->description } } };
            rule["defaultConfiguration"] = Json { { "level", std::string { sarif_level(known->severity) } } };
        }
        ruleIndex.emplace(std::string { id }, rulesJson.size());
        rulesJson.push_back(std::move(rule));
        return rulesJson.size() - 1;
    };
    Json results = Json::array();
    for (const auto& finding : findings) {
        Json related = Json::array();
        for (std::size_t i { 0 }; i < finding.evidence.size(); ++i) {
            const auto& evidence = finding.evidence[i];
            related.push_back(Json { { "id", static_cast<int>(i + 1) },
                                     { "message", Json { { "text", evidence.detail.empty() ? std::format("{} ({})", evidence.id, evidence.kind)
                                                                                        : std::format("{} ({}): {}", evidence.id, evidence.kind, evidence.detail) } } },
                                     { "physicalLocation", physical_location(evidence.location) } });
        }
        Json properties { { "origin", finding.origin }, { "findingId", finding.id } };
        if (finding.model) properties["model"] = Json { { "source", finding.model->source }, { "name", finding.model->name }, { "template", finding.model->templateVersion } };
        if (finding.confidence) properties["confidence"] = *finding.confidence;
        Json result { { "ruleId", finding.rule },
                      { "ruleIndex", index_of(finding.rule) },
                      { "level", std::string { sarif_level(finding.severity) } },
                      { "message", Json { { "text", finding.message } } },
                      { "locations", Json::array({ Json { { "physicalLocation", physical_location(finding.location) } } }) },
                      { "partialFingerprints", Json { { "mcppls/v1", finding.fingerprint.empty() ? spec::fingerprint_of(finding) : finding.fingerprint } } },
                      { "properties", std::move(properties) } };
        if (!related.empty()) result["relatedLocations"] = std::move(related);
        results.push_back(std::move(result));
    }
    std::string rootUri { base::path_to_uri(root) };
    if (!rootUri.ends_with('/')) rootUri += '/';
    return Json {
        { "$schema", "https://json.schemastore.org/sarif-2.1.0.json" },
        { "version", "2.1.0" },
        { "runs", Json::array({ Json {
                      { "tool", Json { { "driver", Json { { "name", "mcppls" },
                                                          { "fullName", "mcpp-language-server review" },
                                                          { "version", std::string { mcppls::base::VERSION } },
                                                          { "informationUri", "https://github.com/Sunrisepeak/mcpp-language-server" },
                                                          { "rules", std::move(rulesJson) } } } } },
                      { "originalUriBaseIds", Json { { "%SRCROOT%", Json { { "uri", rootUri } } } } },
                      { "columnKind", "unicodeCodePoints" },
                      { "properties", Json { { "base", std::string { base } } } },
                      { "results", std::move(results) } } }) },
    };
}

Json to_lsp_diagnostic(const spec::Finding& finding, const std::function<std::string(std::string_view file)>& uri_of) {
    Json related = Json::array();
    for (const auto& evidence : finding.evidence) {
        related.push_back(Json { { "location", Json { { "uri", uri_of(evidence.location.file) }, { "range", lsp_range(evidence.location) } } },
                                 { "message", evidence.detail.empty() ? std::format("{} {}", evidence.id, evidence.kind) : std::format("{} {}: {}", evidence.id, evidence.kind, evidence.detail) } });
    }
    Json diagnostic { { "range", lsp_range(finding.location) },
                      { "severity", spec::lsp_severity(finding.severity) },
                      { "code", finding.rule },
                      { "source", finding.model ? std::format("mcppls review · {}", finding.model->name) : std::string { "mcppls review" } },
                      { "message", finding.message },
                      { "data", Json { { "fingerprint", finding.fingerprint.empty() ? spec::fingerprint_of(finding) : finding.fingerprint }, { "id", finding.id } } } };
    if (!related.empty()) diagnostic["relatedInformation"] = std::move(related);
    return diagnostic;
}

std::string to_markdown(std::span<const spec::Finding> findings, std::string_view base, const Json& summary) {
    std::string out { std::format("# mcppls review against {}\n\n", base) };
    const Json counts = summary.value("counts", Json::object());
    out += std::format("{} error(s), {} warning(s), {} note(s)", counts.value("error", 0), counts.value("warning", 0), counts.value("information", 0));
    if (!summary.value("complete", true)) out += "; incomplete: some units were not searched or built";
    out += "\n\n";
    for (const auto& finding : findings) {
        out += std::format("## {} {} — {}\n\n{}\n\n`{}:{}:{}`\n\n", finding.id, spec::to_string(finding.severity), finding.rule, finding.message, finding.location.file,
                           finding.location.line, finding.location.column);
        if (!finding.location.text.empty()) out += std::format("```cpp\n{}\n```\n\n", finding.location.text);
        for (const auto& evidence : finding.evidence) {
            out += std::format("- {} ({}) `{}:{}`: `{}`{}\n", evidence.id, evidence.kind, evidence.location.file, evidence.location.line,
                               base::trim(evidence.location.text), evidence.detail.empty() ? std::string {} : " — " + evidence.detail);
        }
        out += "\n";
    }
    return out;
}

} // namespace mcppls::ai::review
