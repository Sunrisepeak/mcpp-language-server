module mcppls.orchestrator.highlights;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.orchestrator.tokens;

namespace mcppls::orchestrator::highlights {

namespace {

std::optional<base::Position> position_of(const Json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto line = value.find("line");
    const auto character = value.find("character");
    if (line == value.end() || character == value.end() || !line->is_number_integer() || !character->is_number_integer()) return std::nullopt;
    return base::Position { line->get<int>(), character->get<int>() };
}

// The shape of an inserted text: its line breaks, and how long its last line is.
void measure_inserted(std::string_view text, Edit& edit) {
    edit.lines = static_cast<int>(std::ranges::count(text, '\n'));
    const auto last = text.rfind('\n');
    edit.lastLength = static_cast<int>(base::utf16_length(last == std::string_view::npos ? text : text.substr(last + 1)));
}

// The text a range change leaves, for a later change of the same notification that has no range.
void apply_range(std::string& text, base::Position start, base::Position end, std::string_view inserted) {
    const auto offset = [&](base::Position position) { return base::offset_at(text, position).value_or(text.size()); };
    std::size_t from { offset(start) };
    std::size_t to { offset(end) };
    if (to < from) std::swap(from, to);
    text.replace(from, to - from, inserted);
}

bool continuation_byte(char c) { return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u; }

// The edit a whole new text makes: what lies between the texts' common beginning and common end, neither of
// which splits a code point or a CR LF pair.
Edit replacement(std::string_view before, std::string_view after) {
    std::size_t prefix { 0 };
    const std::size_t shorter { std::min(before.size(), after.size()) };
    while (prefix < shorter && before[prefix] == after[prefix]) ++prefix;
    while (prefix > 0 && prefix < before.size() && (continuation_byte(before[prefix]) || (before[prefix] == '\n' && before[prefix - 1] == '\r'))) --prefix;
    std::size_t suffix { 0 };
    while (suffix < shorter - prefix && before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) ++suffix;
    while (suffix > 0 && (continuation_byte(before[before.size() - suffix])
                          || (before[before.size() - suffix] == '\n' && before.size() > suffix && before[before.size() - suffix - 1] == '\r'))) --suffix;
    Edit edit;
    edit.start = base::position_at(before, prefix);
    edit.end = base::position_at(before, before.size() - suffix);
    measure_inserted(after.substr(prefix, after.size() - suffix - prefix), edit);
    return edit;
}

} // namespace

std::vector<Edit> edits_of(std::string_view before, const Json& contentChanges) {
    std::vector<Edit> edits;
    if (!contentChanges.is_array()) return edits;
    // The text as the changes so far left it: needed only by a change without a range that is not the first.
    std::optional<std::string> working;
    for (std::size_t i { 1 }; i < contentChanges.size(); ++i) {
        if (contentChanges[i].is_object() && !contentChanges[i].contains("range")) {
            working = std::string { before };
            break;
        }
    }
    for (const auto& change : contentChanges) {
        if (!change.is_object()) continue;
        const auto text = change.find("text");
        if (text == change.end() || !text->is_string()) continue;
        const std::string& inserted { text->get_ref<const std::string&>() };
        const auto range = change.find("range");
        if (range != change.end() && range->is_object()) {
            const auto start = position_of(range->value("start", Json {}));
            const auto end = position_of(range->value("end", Json {}));
            if (!start || !end) continue;
            Edit edit;
            edit.start = std::min(*start, *end);
            edit.end = std::max(*start, *end);
            measure_inserted(inserted, edit);
            edits.push_back(edit);
            if (working) apply_range(*working, edit.start, edit.end, inserted);
        } else {
            edits.push_back(replacement(working ? std::string_view { *working } : before, inserted));
            if (working) *working = inserted;
        }
    }
    return edits;
}

void shift(std::vector<Token>& tokens, const Edit& edit) {
    const int lineDelta { edit.lines - (edit.end.line - edit.start.line) };
    std::size_t kept { 0 };
    for (std::size_t i { 0 }; i < tokens.size(); ++i) {
        Token token { tokens[i] };
        const base::Position start { token.line, token.startChar };
        const base::Position end { token.line, token.startChar + token.length };
        if (end < edit.start) {
            tokens[kept++] = token;   // before the edit
        } else if (start > edit.end) {
            if (token.line == edit.end.line) {
                token.startChar = (edit.lines == 0 ? edit.start.character + edit.lastLength : edit.lastLength) + (token.startChar - edit.end.character);
            }
            token.line += lineDelta;
            tokens[kept++] = token;   // after it: moved with the text
        }
        // else: the edit touches it (an insertion at either end included); the compiler says what it is now.
    }
    tokens.resize(kept);
}

std::vector<Token> within(std::span<const Token> tokens, const base::Range& range) {
    std::vector<Token> out;
    for (const auto& token : tokens) {
        const bool afterStart { token.line > range.start.line || (token.line == range.start.line && token.startChar + token.length > range.start.character) };
        const bool beforeEnd { token.line < range.end.line || (token.line == range.end.line && token.startChar < range.end.character) };
        if (afterStart && beforeEnd) out.push_back(token);
    }
    return out;
}

std::uint64_t digest(std::span<const Token> tokens) {
    std::uint64_t value { 1469598103934665603ULL };
    const auto mix = [&](std::uint64_t part) {
        for (int i { 0 }; i < 8; ++i) {
            value ^= (part >> (i * 8)) & 0xFFu;
            value *= 1099511628211ULL;
        }
    };
    mix(tokens.size());
    for (const auto& token : tokens) {
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(token.line)) << 32 | static_cast<std::uint32_t>(token.startChar));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(token.length)) << 32 | static_cast<std::uint32_t>(token.type));
        mix(token.modifiers);
    }
    return value;
}

Kept* Store::find(std::string_view uri) {
    const auto it = kept_.find(uri);
    return it == kept_.end() ? nullptr : &it->second;
}

Kept& Store::at(std::string_view uri) {
    if (const auto it = kept_.find(uri); it != kept_.end()) return it->second;
    return kept_.emplace(std::string { uri }, Kept {}).first->second;
}

void Store::forget(std::string_view uri) {
    if (const auto it = kept_.find(uri); it != kept_.end()) kept_.erase(it);
    if (const auto it = log_.find(uri); it != log_.end()) log_.erase(it);
}

void Store::edited(std::string_view uri, std::int64_t before, std::int64_t version, std::span<const Edit> edits) {
    Kept* kept { find(uri) };
    if (kept == nullptr) return;
    for (const auto& edit : edits) shift(kept->core, edit);
    kept->version = version;
    kept->exact = false;
    auto& log = log_[std::string { uri }];
    log.push_back(Change { before, version, { edits.begin(), edits.end() } });
    while (log.size() > LOG_LIMIT) log.pop_front();
}

bool Store::engine_answered(std::string_view uri, std::int64_t answered, std::int64_t current, std::vector<Token> core, bool settled) {
    Kept& kept { at(uri) };
    if (answered != current) {
        // The edits since `answered`, in order: known only when the log reaches back to it.
        const auto log = log_.find(uri);
        if (log == log_.end()) {
            ++counters.dropped;
            return false;
        }
        const auto first = std::ranges::find(log->second, answered, &Change::before);
        if (first == log->second.end()) {
            ++counters.dropped;
            return false;
        }
        for (auto change = first; change != log->second.end(); ++change) {
            for (const auto& edit : change->edits) shift(core, edit);
        }
    }
    kept.core = std::move(core);
    kept.known = true;
    kept.version = current;
    kept.exact = answered == current && settled;
    return true;
}

void Store::unsettle_all() {
    for (auto& [uri, kept] : kept_) kept.exact = false;
}

void Store::unsettle_except(std::string_view uri) {
    for (auto& [other, kept] : kept_) {
        if (other != uri) kept.exact = false;
    }
}

void Store::composed(double milliseconds) {
    counters.composeMs.push_back(milliseconds);
    while (counters.composeMs.size() > 256) counters.composeMs.pop_front();
}

Json Store::report() const {
    std::vector<double> sorted { counters.composeMs.begin(), counters.composeMs.end() };
    std::ranges::sort(sorted);
    const auto at = [&](double share) { return sorted.empty() ? 0.0 : sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(share * static_cast<double>(sorted.size())))]; };
    return Json { { "answers", Json { { "exact", counters.exact }, { "standIn", counters.standIn }, { "waited", counters.waited } } },
                  { "refreshes", counters.refreshes },
                  { "dropped", counters.dropped },
                  { "documents", kept_.size() },
                  { "composeMs", Json { { "p50", at(0.5) }, { "max", sorted.empty() ? 0.0 : sorted.back() } } } };
}

} // namespace mcppls::orchestrator::highlights
