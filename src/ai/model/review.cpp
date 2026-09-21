module mcppls.ai.model.review;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.platform.fs;
import mcppls.spec.query;
import mcppls.ai.model.source;
import mcppls.ai.model.prompt;
import mcppls.ai.model.schema;

namespace mcppls::ai::model {

namespace {

// Left for the review layer's own fix verification (design 7.4 step 6); the model step never
// spends more than this on an answer regardless of how large the input budget is.
inline constexpr int MAX_OUTPUT_TOKENS { 2048 };
// design 7.2/7.5: token counts are estimated at 4 characters per token.
inline constexpr std::size_t CHARACTERS_PER_TOKEN { 4 };

std::string cache_file(const std::string& directory, std::string_view key) { return base::join_path(directory, std::string { key } + ".json"); }

} // namespace

Cache::Cache(std::string directory) : directory_ { std::move(directory) } { }

std::optional<Json> Cache::get(std::string_view key) const {
    if (directory_.empty()) return std::nullopt;
    auto text = platform::fs::read_file(cache_file(directory_, key));
    if (!text) return std::nullopt;
    Json value = Json::parse(*text, nullptr, false);
    if (value.is_discarded()) return std::nullopt;
    return value;
}

void Cache::put(std::string_view key, const Json& value) const {
    if (directory_.empty()) return;
    (void)platform::fs::create_directories(directory_);
    (void)platform::fs::write_file_atomic(cache_file(directory_, key), value.dump());
}

std::string review_cache_key(std::string_view templateVersion, std::string_view model, const std::vector<Message>& messages) {
    base::Sha256 digest;
    auto field = [&](std::string_view value) {
        digest.update(std::format("{}:", value.size()));
        digest.update(value);
    };
    field(templateVersion);
    field(model);
    for (const auto& message : messages) {
        field(message.role);
        field(message.content);
    }
    return digest.finish();
}

ReviewResult model_findings(ModelClient& client, const ChangeContext& context, const ModelSettings& settings, Cache& cache, VerifyFix verifyFix) {
    ReviewResult out;
    if (!settings.explicitlyEnabled) return out;   // M6: nothing rendered, nothing sent
    out.enabled = true;

    const std::vector<Message> messages { render_review_prompt(context, settings) };
    std::size_t characters { 0 };
    for (const auto& message : messages) characters += message.role.size() + message.content.size();
    out.estimatedTokens = (characters + CHARACTERS_PER_TOKEN - 1) / CHARACTERS_PER_TOKEN;
    if (out.estimatedTokens > settings.tokenBudget) {
        out.budgetExceeded = true;
        return out;
    }

    const std::string key { review_cache_key(REVIEW_TEMPLATE_VERSION, settings.model, messages) };
    CompletionResult completion;
    if (auto cached = cache.get(key)) {
        out.cacheHit = true;
        completion.output = cached->value("output", Json {});
        completion.modelName = cached->value("modelName", settings.model);
        if (const auto usage = cached->find("usage"); usage != cached->end() && usage->is_object()) {
            completion.usage.inputTokens = usage->value("inputTokens", 0);
            completion.usage.outputTokens = usage->value("outputTokens", 0);
        }
    } else {
        CompletionRequest request;
        request.model = settings.model;
        request.messages = messages;
        request.schema = review_output_schema();
        request.maxTokens = MAX_OUTPUT_TOKENS;
        auto result = client.complete(request);
        if (!result) {
            out.error = result.error().message;
            return out;
        }
        completion = *result;
        const Json record { { "output", completion.output }, { "modelName", completion.modelName },
                            { "usage", Json { { "inputTokens", completion.usage.inputTokens }, { "outputTokens", completion.usage.outputTokens } } } };
        cache.put(key, record);
    }
    out.modelName = completion.modelName;
    out.usage = completion.usage;

    if (auto errors = validate(review_output_schema(), completion.output); !errors.empty()) {
        for (const auto& error : errors) out.schemaErrors.push_back(error.path.empty() ? error.message : std::format("{}: {}", error.path, error.message));
        return out;
    }

    std::map<std::string, const spec::Evidence*> byId;
    for (const auto& item : context.evidence) byId[item.id] = &item;

    int nextId { 1 };
    for (const auto& item : completion.output.value("findings", Json::array())) {
        std::vector<spec::Evidence> resolved;
        bool ok { true };
        for (const auto& citedId : item.value("evidence", Json::array())) {
            const auto found = citedId.is_string() ? byId.find(citedId.get<std::string>()) : byId.end();
            if (found == byId.end()) { ok = false; break; }
            resolved.push_back(*found->second);
        }
        if (!ok || resolved.empty()) {
            out.droppedReasons.push_back("dropped a finding whose evidence is empty or cites an id not in this review's context");
            continue;
        }

        spec::Finding finding;
        finding.id = std::format("F{}", nextId++);
        finding.rule = item.value("rule", std::string {});
        finding.severity = spec::parse_severity(item.value("severity", std::string {})).value_or(spec::Severity::warning);
        finding.message = item.value("message", std::string {});
        const Json location = item.value("location", Json::object());
        finding.location.file = location.value("file", std::string {});
        finding.location.line = location.value("line", 0);
        finding.location.column = location.value("column", 1);
        for (const auto& evidence : resolved) {
            if (evidence.location.file == finding.location.file && evidence.location.line == finding.location.line) {
                finding.location.text = evidence.location.text;
                break;
            }
        }
        finding.evidence = std::move(resolved);
        finding.origin = "model";
        finding.model = spec::ModelOrigin { std::string { to_string(settings.source) }, completion.modelName, std::string { REVIEW_TEMPLATE_VERSION } };
        finding.confidence = item.value("confidence", 0.0);
        if (const auto fix = item.find("fix"); fix != item.end() && fix->is_object() && verifyFix && verifyFix(*fix)) finding.fix = *fix;
        finding.fingerprint = spec::fingerprint_of(finding);
        out.findings.push_back(std::move(finding));
    }
    return out;
}

Json to_json(const ContextExplanation& explanation) {
    Json files = Json::array();
    for (const auto& range : explanation.included) files.push_back(Json { { "file", range.file }, { "line", range.line }, { "endLine", range.endLine } });
    return Json { { "enabled", explanation.enabled }, { "files", std::move(files) }, { "excludedFiles", explanation.excludedFiles } };
}

ContextExplanation explain_context(const ChangeContext& context, const ModelSettings& settings) {
    ContextExplanation explanation;
    explanation.enabled = settings.explicitlyEnabled;
    auto filtered = filter_evidence(context, settings.excludedPaths);
    explanation.included = std::move(filtered.included);
    explanation.excludedFiles = std::move(filtered.excludedFiles);
    return explanation;
}

} // namespace mcppls::ai::model
