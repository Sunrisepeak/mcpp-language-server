module mcppls.ai.review.judgement;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.spec.query;
import mcppls.ai.model.source;
import mcppls.ai.model.prompt;
import mcppls.ai.model.review;
import mcppls.ai.review.changes;
import mcppls.ai.review.pipeline;

namespace mcppls::ai::review {

namespace {

using Json = nlohmann::json;

std::string line_of(const std::optional<std::string>& text, int line) {
    return text ? spec::line_of(*text, line - 1) : std::string {};
}

} // namespace

model::ChangeContext change_context(const ReviewResult& result) {
    model::ChangeContext context;
    const Json full = impact_json(result);
    context.semanticDiff = full.value("diffs", Json::array());
    context.impact = full.value("impact", Json::object());
    std::size_t next { 1 };
    // The findings keep their evidence, renumbered so that one id names one item in the whole context.
    for (auto finding : result.findings) {
        for (auto& evidence : finding.evidence) {
            evidence.id = std::format("E{}", next++);
            context.evidence.push_back(evidence);
        }
        context.deterministicFindings.push_back(std::move(finding));
    }
    // Every line the change added or altered, so a model can point at code no rule looked at.
    for (const auto& file : result.changes.files) {
        if (!file.head) continue;
        std::string display { file.path };
        if (auto relative = base::relative_path(file.path, result.changes.repositoryRoot)) display = *relative;
        std::ranges::replace(display, '\\', '/');
        for (const auto& hunk : file.hunks) {
            for (int line { hunk.headStart }; line < hunk.headStart + hunk.headCount; ++line) {
                const std::string text { line_of(file.head, line) };
                context.evidence.push_back(spec::Evidence { std::format("E{}", next++), "diff", spec::Location { display, line, 1, text }, "+" });
            }
            // What went away, at the line it went away before.
            for (int line { hunk.baseStart }; line < hunk.baseStart + hunk.baseCount; ++line) {
                const std::string text { line_of(file.base, line) };
                context.evidence.push_back(spec::Evidence { std::format("E{}", next++), "diff", spec::Location { display, std::max(hunk.headStart, 1), 1, text }, "-" });
            }
        }
    }
    return context;
}

Judgement judge(const ReviewResult& result, const JudgementOptions& options, model::ModelClient* client) {
    Judgement judgement;
    judgement.source = std::string { model::to_string(options.settings.source) };
    const model::ChangeContext context { change_context(result) };
    if (options.explainOnly) {
        judgement.explanation = model::explain_context(context, options.settings);
        return judgement;
    }
    if (options.settings.source == model::SourceKind::none) return judgement;
    if (options.settings.source == model::SourceKind::agent) {
        // Nothing is called and nothing leaves the process: the agent that asked is the model.
        judgement.enabled = true;
        Json messages = Json::array();
        for (const auto& message : model::render_review_prompt(context, options.settings)) messages.push_back(Json { { "role", message.role }, { "content", message.content } });
        judgement.agentContext = Json { { "template", std::string { model::REVIEW_TEMPLATE_VERSION } }, { "messages", std::move(messages) },
                                        { "outputSchema", model::review_output_schema() } };
        return judgement;
    }
    if (client == nullptr) {
        judgement.error = std::format("the {} model source is not available here", judgement.source);
        return judgement;
    }
    model::Cache cache { options.cacheDirectory };
    auto reviewed = model::model_findings(*client, context, options.settings, cache, options.verifyFix);
    judgement.enabled = reviewed.enabled;
    judgement.budgetExceeded = reviewed.budgetExceeded;
    judgement.cacheHit = reviewed.cacheHit;
    judgement.error = reviewed.error;
    judgement.schemaErrors = std::move(reviewed.schemaErrors);
    judgement.droppedReasons = std::move(reviewed.droppedReasons);
    judgement.modelName = reviewed.modelName;
    judgement.usage = reviewed.usage;
    std::size_t next { result.findings.size() + 1 };
    for (auto& finding : reviewed.findings) {
        finding.id = std::format("F{}", next++);
        if (finding.fingerprint.empty()) finding.fingerprint = spec::fingerprint_of(finding);
        judgement.findings.push_back(std::move(finding));
    }
    return judgement;
}

Json to_json(const Judgement& judgement) {
    Json findings = Json::array();
    for (const auto& finding : judgement.findings) findings.push_back(spec::to_json(finding));
    Json value { { "source", judgement.source }, { "enabled", judgement.enabled }, { "budgetExceeded", judgement.budgetExceeded },
                 { "cacheHit", judgement.cacheHit }, { "schemaErrors", judgement.schemaErrors }, { "droppedReasons", judgement.droppedReasons },
                 { "findings", std::move(findings) },
                 { "usage", Json { { "inputTokens", judgement.usage.inputTokens }, { "outputTokens", judgement.usage.outputTokens } } } };
    if (!judgement.modelName.empty()) value["model"] = judgement.modelName;
    if (judgement.error) value["error"] = *judgement.error;
    if (judgement.explanation) value["explanation"] = model::to_json(*judgement.explanation);
    if (!judgement.agentContext.is_null()) value["agentContext"] = judgement.agentContext;
    return value;
}

} // namespace mcppls::ai::review
