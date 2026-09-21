module mcppls.ai.query.view;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.platform.fs;
import mcppls.lsp.jsonrpc;
import mcppls.spec.query;
import mcppls.project.scan;
import mcppls.engine.native.index;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;

namespace mcppls::ai::query {

namespace {

constexpr std::array<std::string_view, 27> SYMBOL_KINDS { "", "file", "module", "namespace", "package", "class", "method", "property", "field",
                                                          "constructor", "enum", "interface", "function", "variable", "constant", "string",
                                                          "number", "boolean", "array", "object", "key", "null", "enum-member", "struct",
                                                          "event", "operator", "type-parameter" };

std::optional<base::Position> position_of(const Json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto line = lsp::int_at(value, "line");
    const auto character = lsp::int_at(value, "character");
    if (!line || !character) return std::nullopt;
    return base::Position { static_cast<int>(*line), static_cast<int>(*character) };
}

} // namespace

Failure invalid_arguments(std::string message) { return Failure { "invalid-arguments", std::move(message), nullptr }; }

Failure not_found(std::string message) { return Failure { "not-found", std::move(message), nullptr }; }

View::View(orchestrator::Kernel& kernel) : kernel_ { kernel } {}

const std::string& View::root() const { return kernel_.root(); }

std::string View::path_of(std::string_view file) const {
    const std::string absolute { kernel_.absolute_path(file) };
    if (absolute.empty()) return {};
    return platform::fs::exists(absolute) ? platform::fs::canonical_path(absolute) : absolute;
}

std::string View::display(std::string_view path) const {
    if (auto relative = base::relative_path(path, root()); relative && base::is_within(path, root())) {
        std::string name { *relative };
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) std::ranges::replace(name, '\\', '/');
        return name;
    }
    return std::string { path };
}

std::string View::text_of(std::string_view path) const { return kernel_.text(path).value_or(std::string {}); }

std::string View::module_of(std::string_view path) const {
    const auto* scan = kernel_.workspace().module_index().scan_of(path);
    if (scan == nullptr) {
        auto text = kernel_.text(path);
        if (!text) return {};
        const auto scanned = project::scan_source(*text);
        if (!scanned.declaration) return {};
        return scanned.declaration->partition.empty() ? scanned.declaration->module : scanned.declaration->module + ":" + scanned.declaration->partition;
    }
    if (!scan->declaration) return {};
    return scan->declaration->partition.empty() ? scan->declaration->module : scan->declaration->module + ":" + scan->declaration->partition;
}

spec::Location View::location(std::string_view path, const Json& range) const {
    const auto start = position_of(range.value("start", Json {}));
    const auto end = position_of(range.value("end", Json {}));
    return spec::location_in(text_of(path), display(path), start.value_or(base::Position {}), end);
}

std::optional<std::string> View::path_of_uri(std::string_view uri) const {
    auto path = base::uri_to_path(uri);
    if (!path) return std::nullopt;
    return platform::fs::exists(*path) ? platform::fs::canonical_path(*path) : base::normalize_path(*path);
}

std::optional<spec::Location> View::location(const Json& lspLocation) const {
    if (!lspLocation.is_object()) return std::nullopt;
    const bool link { lspLocation.contains("targetUri") };
    const auto uri = lsp::string_at(lspLocation, link ? "targetUri" : "uri");
    if (!uri) return std::nullopt;
    const auto path = path_of_uri(*uri);
    if (!path) return std::nullopt;
    const Json range = lspLocation.value(link ? "targetSelectionRange" : "range", Json::object());
    return location(*path, range);
}

Json View::lsp_position(std::string_view path, int line, int column) const {
    const auto position = spec::lsp_position(text_of(path), line, column);
    return Json { { "line", position.line }, { "character", position.character } };
}

Json View::text_document(std::string_view path) const { return Json { { "uri", kernel_.uri_of(path) } }; }

spec::Snapshot View::snapshot() const {
    spec::Snapshot snapshot;
    snapshot.generation = kernel_.workspace().snapshot_generation();
    for (const auto& path : kernel_.overlays()) snapshot.overlays.push_back(display(path));
    const Json status = kernel_.status();
    snapshot.preparing = status.is_object() && (status.value("state", std::string {}) == "preparing" || status.value("state", std::string {}) == "loading"
                                                || status.value("state", std::string {}) == "starting");
    snapshot.indexing = kernel_.indexing();
    return snapshot;
}

void View::refresh() { (void)kernel_.refresh(); }

bool View::settle(Clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return kernel_.wait_settled(std::max(remaining, std::chrono::milliseconds { 0 }));
}

void View::settle_index(Clock::time_point deadline) {
    if (!has_core_engine()) return;
    auto remaining = [](Clock::time_point until) {
        return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()), std::chrono::milliseconds { 0 });
    };
    if (kernel_.progress_begun() == 0) {
        // clangd reads its database, and starts indexing what it names, when a file is first opened.
        const auto& entries = kernel_.workspace().engine_plan().entries;
        const auto unit = std::ranges::find_if(entries, [this](const auto& entry) { return base::is_within(entry.file, root()); });
        if (unit != entries.end()) open(unit->file);
        // A report begins shortly after; when none has begun within a few seconds there is no index to wait for.
        const auto firstReport = std::min(deadline, Clock::now() + std::chrono::seconds { 5 });
        (void)kernel_.wait_until([this] { return kernel_.progress_begun() > 0; }, remaining(firstReport));
    }
    (void)kernel_.wait_until([this] { return !kernel_.indexing(); }, remaining(deadline));
}

std::optional<Json> View::request(std::string_view method, Json params, Clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    auto response = kernel_.request(method, std::move(params), std::max(remaining, std::chrono::milliseconds { 1 }));
    if (!response) return std::nullopt;
    if (response->contains("error")) return Json(nullptr);
    return response->value("result", Json {});
}

void View::open(std::string_view path) { kernel_.open(path); }

bool View::has_core_engine() const { return kernel_.workspace().core_engine_status().has_value(); }

std::string_view symbol_kind_name(int kind) {
    if (kind < 1 || kind >= static_cast<int>(SYMBOL_KINDS.size())) return "unknown";
    return SYMBOL_KINDS[static_cast<std::size_t>(kind)];
}

std::optional<int> symbol_kind_from_name(std::string_view name) {
    for (std::size_t i { 1 }; i < SYMBOL_KINDS.size(); ++i) {
        if (SYMBOL_KINDS[i] == name) return static_cast<int>(i);
    }
    return std::nullopt;
}

Json to_json(const Failure& failure) {
    Json value { { "code", failure.code }, { "message", failure.message } };
    if (!failure.candidates.is_null()) value["candidates"] = failure.candidates;
    return value;
}

} // namespace mcppls::ai::query
