module mcppls.orchestrator.documents;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.base.path;

namespace mcppls::orchestrator {

namespace {

std::optional<base::Position> read_position(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto line = value.find("line");
    const auto character = value.find("character");
    if (line == value.end() || character == value.end() || !line->is_number_integer() || !character->is_number_integer()) return std::nullopt;
    return base::Position { line->get<int>(), character->get<int>() };
}

} // namespace

void apply_change(std::string& text, const nlohmann::json& change) {
    const auto newText = change.find("text");
    if (newText == change.end() || !newText->is_string()) return;
    const auto range = change.find("range");
    if (range == change.end() || !range->is_object()) {
        text = newText->get<std::string>();
        return;
    }
    const auto start = read_position(range->value("start", nlohmann::json {}));
    const auto end = read_position(range->value("end", nlohmann::json {}));
    if (!start || !end) return;
    const auto clamp = [&](base::Position position) -> std::size_t {
        if (auto offset = base::offset_at(text, position)) return *offset;
        return text.size();   // positions past the end clamp to it
    };
    std::size_t from { clamp(*start) };
    std::size_t to { clamp(*end) };
    if (to < from) std::swap(from, to);
    text.replace(from, to - from, newText->get<std::string>());
}

Document& DocumentStore::open(std::string uri, std::string path, std::string languageId, std::int64_t version, std::string text) {
    Document document { uri, std::move(path), std::move(languageId), version, std::move(text) };
    auto [it, inserted] = documents_.insert_or_assign(std::move(uri), std::move(document));
    return it->second;
}

bool DocumentStore::change(std::string_view uri, std::int64_t version, const nlohmann::json& contentChanges) {
    const auto it = documents_.find(uri);
    if (it == documents_.end()) return false;
    if (contentChanges.is_array()) {
        for (const auto& change : contentChanges) apply_change(it->second.text, change);
    }
    it->second.version = version;
    return true;
}

void DocumentStore::close(std::string_view uri) {
    if (const auto it = documents_.find(uri); it != documents_.end()) documents_.erase(it);
}

const Document* DocumentStore::find(std::string_view uri) const {
    const auto it = documents_.find(uri);
    return it == documents_.end() ? nullptr : &it->second;
}

const Document* DocumentStore::find_by_path(std::string_view path) const {
    for (const auto& [uri, document] : documents_) {
        if (!document.path.empty() && base::same_path(document.path, path)) return &document;
    }
    return nullptr;
}

std::vector<const Document*> DocumentStore::all() const {
    std::vector<const Document*> result;
    for (const auto& [uri, document] : documents_) result.push_back(&document);
    return result;
}

} // namespace mcppls::orchestrator
