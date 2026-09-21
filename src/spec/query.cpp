module mcppls.spec.query;

import std;
import nlohmann.json;
import mcppls.base.sha256;
import mcppls.base.text;

namespace mcppls::spec {

using Json = nlohmann::json;

namespace {

std::size_t sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;   // a stray continuation byte counts as one scalar value, as a decoder replacing it would
}

std::string without_line_end(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return std::string { line };
}

// Whitespace runs collapsed, so reindenting a line keeps its fingerprint.
std::string normalized(std::string_view text) {
    std::string out;
    bool space { false };
    for (char c : base::trim(text)) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

} // namespace

std::string_view to_string(Severity severity) {
    switch (severity) {
    case Severity::error: return "error";
    case Severity::warning: return "warning";
    case Severity::information: return "information";
    case Severity::hint: return "hint";
    }
    return "warning";
}

std::optional<Severity> parse_severity(std::string_view text) {
    if (text == "error") return Severity::error;
    if (text == "warning") return Severity::warning;
    if (text == "information" || text == "info" || text == "note") return Severity::information;
    if (text == "hint") return Severity::hint;
    return std::nullopt;
}

int lsp_severity(Severity severity) { return static_cast<int>(severity) + 1; }

Severity severity_from_lsp(int severity) {
    if (severity < 1 || severity > 4) return Severity::error;   // LSP: a missing severity is up to the client; an error is the safe reading
    return static_cast<Severity>(severity - 1);
}

std::string line_of(std::string_view text, int zeroBasedLine) {
    if (zeroBasedLine < 0) return {};
    std::size_t start { 0 };
    for (int line { 0 }; line < zeroBasedLine; ++line) {
        const std::size_t at { text.find('\n', start) };
        if (at == std::string_view::npos) return {};
        start = at + 1;
    }
    const std::size_t end { text.find('\n', start) };
    return without_line_end(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

int column_from_utf16(std::string_view lineText, int character) {
    int column { 1 };
    int units { 0 };
    std::size_t i { 0 };
    while (i < lineText.size() && units < character) {
        const std::size_t n { std::min(sequence_length(static_cast<unsigned char>(lineText[i])), lineText.size() - i) };
        units += n == 4 ? 2 : 1;
        i += n;
        ++column;
    }
    // Past the end of the line: one column per unit, as if the line went on.
    if (units < character) column += character - units;
    return column;
}

int utf16_from_column(std::string_view lineText, int column) {
    int units { 0 };
    int current { 1 };
    std::size_t i { 0 };
    while (i < lineText.size() && current < column) {
        const std::size_t n { std::min(sequence_length(static_cast<unsigned char>(lineText[i])), lineText.size() - i) };
        units += n == 4 ? 2 : 1;
        i += n;
        ++current;
    }
    if (current < column) units += column - current;
    return units;
}

Location location_in(std::string_view text, std::string file, base::Position start, std::optional<base::Position> end) {
    Location location;
    location.file = std::move(file);
    location.line = start.line + 1;
    location.text = line_of(text, start.line);
    location.column = column_from_utf16(location.text, start.character);
    if (end && *end != start) {
        location.endLine = end->line + 1;
        location.endColumn = column_from_utf16(end->line == start.line ? location.text : line_of(text, end->line), end->character);
    }
    return location;
}

base::Position lsp_position(std::string_view text, int line, int column) {
    const int zeroBased { std::max(line, 1) - 1 };
    return base::Position { zeroBased, utf16_from_column(line_of(text, zeroBased), std::max(column, 1)) };
}

std::string fingerprint_of(const Finding& finding) {
    base::Sha256 digest;
    auto field = [&](std::string_view value) {
        digest.update(std::format("{}:", value.size()));
        digest.update(value);
    };
    field(finding.rule);
    field(finding.location.file);
    field(normalized(finding.location.text));
    for (const auto& evidence : finding.evidence) {
        field(evidence.kind);
        field(evidence.location.file);
        field(normalized(evidence.location.text));
    }
    return "sha256:" + digest.finish();
}

Json to_json(const Location& location) {
    Json value { { "file", location.file }, { "line", location.line }, { "column", location.column }, { "text", location.text } };
    if (location.endLine) value["endLine"] = *location.endLine;
    if (location.endColumn) value["endColumn"] = *location.endColumn;
    return value;
}

Json to_json(const Snapshot& snapshot) {
    return Json { { "generation", snapshot.generation }, { "overlays", snapshot.overlays }, { "preparing", snapshot.preparing }, { "indexing", snapshot.indexing } };
}

Json to_json(const Evidence& evidence) {
    Json value { { "id", evidence.id }, { "kind", evidence.kind }, { "location", to_json(evidence.location) } };
    if (!evidence.detail.empty()) value["detail"] = evidence.detail;
    return value;
}

Json to_json(const Finding& finding) {
    Json evidence = Json::array();
    for (const auto& item : finding.evidence) evidence.push_back(to_json(item));
    Json value { { "id", finding.id },
                 { "rule", finding.rule },
                 { "severity", std::string { to_string(finding.severity) } },
                 { "message", finding.message },
                 { "location", to_json(finding.location) },
                 { "evidence", std::move(evidence) },
                 { "origin", finding.origin },
                 { "fix", finding.fix },
                 { "fingerprint", finding.fingerprint.empty() ? fingerprint_of(finding) : finding.fingerprint } };
    if (finding.model) {
        value["model"] = Json { { "source", finding.model->source }, { "name", finding.model->name }, { "template", finding.model->templateVersion } };
    }
    if (finding.confidence) value["confidence"] = *finding.confidence;
    return value;
}

std::optional<Location> location_from_json(const Json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto file = value.find("file");
    const auto line = value.find("line");
    if (file == value.end() || !file->is_string() || line == value.end() || !line->is_number_integer()) return std::nullopt;
    Location location;
    location.file = file->get<std::string>();
    location.line = line->get<int>();
    location.column = value.value("column", 1);
    location.text = value.value("text", std::string {});
    if (const auto endLine = value.find("endLine"); endLine != value.end() && endLine->is_number_integer()) location.endLine = endLine->get<int>();
    if (const auto endColumn = value.find("endColumn"); endColumn != value.end() && endColumn->is_number_integer()) location.endColumn = endColumn->get<int>();
    return location;
}

std::optional<Finding> finding_from_json(const Json& value) {
    if (!value.is_object()) return std::nullopt;
    Finding finding;
    finding.id = value.value("id", std::string {});
    finding.rule = value.value("rule", std::string {});
    finding.message = value.value("message", std::string {});
    const auto severity = parse_severity(value.value("severity", std::string {}));
    const auto location = location_from_json(value.value("location", Json {}));
    if (finding.rule.empty() || finding.message.empty() || !severity || !location) return std::nullopt;
    finding.severity = *severity;
    finding.location = *location;
    for (const auto& item : value.value("evidence", Json::array())) {
        const auto evidenceLocation = location_from_json(item.value("location", Json {}));
        if (!item.is_object() || !evidenceLocation) return std::nullopt;
        finding.evidence.push_back(Evidence { item.value("id", std::string {}), item.value("kind", std::string {}), *evidenceLocation, item.value("detail", std::string {}) });
    }
    finding.origin = value.value("origin", std::string { "rule" });
    finding.fix = value.value("fix", Json {});
    finding.fingerprint = value.value("fingerprint", std::string {});
    if (const auto model = value.find("model"); model != value.end() && model->is_object()) {
        finding.model = ModelOrigin { model->value("source", std::string {}), model->value("name", std::string {}), model->value("template", std::string {}) };
    }
    if (const auto confidence = value.find("confidence"); confidence != value.end() && confidence->is_number()) finding.confidence = confidence->get<double>();
    return finding;
}

} // namespace mcppls::spec
