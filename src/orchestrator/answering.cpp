module mcppls.orchestrator.answering;

import std;
import nlohmann.json;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.uri;
import mcppls.orchestrator.answer;
import mcppls.orchestrator.outline;
import mcpplibs.os;

namespace os = mcpplibs::os;

namespace mcppls::orchestrator::answering {

namespace log = base::log;

namespace {

constexpr int FILE_FORMAT { 1 };
constexpr std::size_t PERSISTED_BYTES { 8u << 20 };   // what a file keeps at most (the memory store keeps more)
constexpr std::chrono::seconds SAVE_EVERY { 60 };

std::uint64_t mix(std::uint64_t seed, std::string_view text) {
    const std::uint64_t value { std::hash<std::string_view> {}(text) };
    return seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void push_ms(std::deque<double>& into, std::chrono::milliseconds took) {
    into.push_back(static_cast<double>(took.count()));
    if (into.size() > 512) into.pop_front();
}

Json percentiles(const std::deque<double>& values) {
    if (values.empty()) return Json::object();
    std::vector<double> sorted { values.begin(), values.end() };
    std::ranges::sort(sorted);
    const auto at = [&](double p) { return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5))]; };
    return Json { { "n", sorted.size() }, { "p50Ms", at(0.5) }, { "p95Ms", at(0.95) }, { "p99Ms", at(0.99) }, { "maxMs", sorted.back() } };
}

// The first location of a jump's answer: its file's path (from a file: URI), line and column.
std::optional<std::tuple<std::string, int, int>> first_location(const Json& result) {
    const Json* location { result.is_array() ? (result.empty() ? nullptr : &result[0]) : result.is_object() ? &result : nullptr };
    if (location == nullptr || !location->is_object()) return std::nullopt;
    const std::string uri { location->contains("targetUri") ? location->value("targetUri", std::string {}) : location->value("uri", std::string {}) };
    const Json* range { location->contains("targetSelectionRange") ? &(*location)["targetSelectionRange"] : location->contains("range") ? &(*location)["range"] : nullptr };
    if (uri.empty() || range == nullptr || !range->is_object() || !range->contains("start")) return std::nullopt;
    auto path = base::uri_to_path(uri);
    if (!path) return std::nullopt;
    return std::tuple { *path, (*range)["start"].value("line", 0), (*range)["start"].value("character", 0) };
}

bool simple_name(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; });
}

} // namespace

std::string fingerprint(std::string_view toolchain, std::span<const std::string> arguments, std::span<const std::string> imports, std::string_view text) {
    std::uint64_t value { mix(0, toolchain) };
    for (const auto& argument : arguments) value = mix(value, argument);
    std::vector<std::string> sorted { imports.begin(), imports.end() };
    std::ranges::sort(sorted);
    for (const auto& name : sorted) value = mix(value, name);
    // The preamble: the lines a file begins with that are directives, comments, blank, or `module;`.
    std::size_t end { 0 };
    bool comment { false };
    while (end < text.size()) {
        const std::size_t next { std::min(text.find('\n', end), text.size()) };
        const std::string_view line { base::trim(text.substr(end, next - end)) };
        const bool preamble { comment || line.empty() || line.starts_with('#') || line.starts_with("//") || line.starts_with("/*") || line == "module;" };
        if (line.starts_with("/*") && line.find("*/") == std::string_view::npos) comment = true;
        else if (comment && line.find("*/") != std::string_view::npos) comment = false;
        if (!preamble) break;
        end = next + 1;
    }
    value = mix(value, text.substr(0, std::min(end, text.size())));
    return std::format("{:016x}", value);
}

Layer::Layer(Settings settings, std::string workspaceDirectory, std::string namesDirectory,
             std::function<std::optional<std::string>(std::string_view)> openText)
    : settings_ { std::move(settings) },
      workspaceFile_ { workspaceDirectory.empty() ? std::string {} : base::join_path(workspaceDirectory, "answers.json") },
      namesDirectory_ { std::move(namesDirectory) }, openText_ { std::move(openText) } {}

void Layer::changed(std::string_view path) {
    ++changes_[base::path_key(path)];
    ++total_;
}

void Layer::everything_changed() { total_ += std::uint64_t { 1 } << 20; }

std::uint64_t Layer::generation_(std::string_view path) const {
    // An open file's text; a closed one's stamp on disk: both the same in the next session when nothing changed.
    if (const auto text = text_of_(path)) return mix(1, *text);
    if (const auto stamp = base::file_stamp(path)) return mix(mix(2, std::to_string(stamp->size)), std::to_string(static_cast<std::int64_t>(stamp->modified_ns)));
    return 0;
}

std::optional<std::string> Layer::text_of_(std::string_view path) const {
    if (openText_) {
        if (auto text = openText_(path)) return text;
    }
    return std::nullopt;
}

answer::Now Layer::now_(const Asked& asked) const {
    const auto own = changes_.find(base::path_key(asked.path));
    return answer::Now { asked.fingerprint, total_ - (own == changes_.end() ? 0 : own->second), asked.context.localHash,
                         [this](std::string_view path) { return generation_(path); } };
}

std::optional<Asked> Layer::read(std::string_view method, std::string uri, std::string path, std::string text, const Json& params,
                                 std::string fingerprint) const {
    if (!enabled()) return std::nullopt;
    const auto kind = answer::kind_of(method);
    if (!kind) return std::nullopt;
    const Json* position { params.is_object() && params.contains("position") ? &params["position"] : nullptr };
    if (position == nullptr || !position->is_object()) return std::nullopt;
    const base::Position at { position->value("line", -1), position->value("character", -1) };
    if (at.line < 0 || at.character < 0) return std::nullopt;
    auto context = answer::context_at(text, at, *kind);
    if (!context || context->place == answer::Context::Place::other) return std::nullopt;
    Asked asked;
    asked.kind = *kind;
    asked.context = std::move(*context);
    asked.uri = std::move(uri);
    asked.path = std::move(path);
    asked.position = at;
    asked.fingerprint = std::move(fingerprint);
    asked.k1 = answer::k1(asked.uri, asked.kind, asked.context);
    const auto& receiver = asked.context.receiver;
    if (asked.context.place == answer::Context::Place::qualified || (asked.context.place == answer::Context::Place::call && receiver.find("::") != std::string::npos)) {
        asked.expanded = receiver == "::" ? receiver : answer::expand_aliases(text, receiver);
        asked.k3 = answer::k3(asked.kind, asked.context, asked.expanded);
    }
    if ((asked.context.place == answer::Context::Place::member || asked.context.place == answer::Context::Place::arrow) && simple_name(receiver)) {
        asked.declaredType = answer::declared_type(text, asked.context.wordStart, receiver);
    }
    asked.text = std::move(text);
    return asked;
}

std::optional<Found> Layer::lookup(const Asked& asked) {
    const answer::Now now { now_(asked) };
    const auto found = [&](const std::string& key, answer::Source source) -> std::optional<Found> {
        const answer::Entry* entry { store_.find(key) };
        if (entry == nullptr || entry->kind != asked.kind) return std::nullopt;
        Found out { *entry, key, source, answer::certainty_of(*entry, source, now), answer::provider_changed(*entry, now) };
        if (answer::is_jump(asked.kind)) {
            // A jump depends on the file it lands in, not on the files beside it.
            out.certainty = entry->fingerprint == now.fingerprint && !out.providerChanged && !entry->provider.empty() ? answer::Certainty::certain
                                                                                                                    : answer::Certainty::uncertain;
        }
        return out;
    };
    if (auto hit = found(asked.k1, answer::Source::k1)) return hit;
    if (asked.kind == answer::Kind::completion || asked.kind == answer::Kind::hover) {
        const Type* type { nullptr };
        if (const auto it = placeTypes_.find(asked.k1); it != placeTypes_.end()) type = &it->second;
        else if (asked.declaredType) {
            if (const auto spelled = spelledTypes_.find(*asked.declaredType); spelled != spelledTypes_.end()) type = &spelled->second;
        }
        if (type != nullptr) {
            if (auto hit = found(answer::k2(asked.kind, asked.context.place, type->key), answer::Source::k2)) return hit;
        }
    }
    if (asked.k3) {
        if (auto hit = found(*asked.k3, answer::Source::k3)) return hit;
    }
    return std::nullopt;
}

std::optional<Served> Layer::serve(const Asked& asked, const Found& found) const {
    Served served;
    served.provenance = answer::Provenance { found.source, found.certainty, std::max<std::int64_t>(0, now_ms() - found.entry.keptAt), found.providerChanged, true, {} };
    switch (asked.kind) {
    case answer::Kind::completion: {
        std::optional<std::vector<std::string>> members;
        const bool memberPlace { asked.context.place == answer::Context::Place::member || asked.context.place == answer::Context::Place::arrow };
        if (memberPlace && found.providerChanged && !found.entry.provider.empty() && found.entry.providerLine >= 0) {
            std::optional<std::string> text { text_of_(found.entry.provider) };
            if (!text) {
                if (auto disk = os::fs::read(found.entry.provider)) text = std::move(*disk);
            }
            if (text) members = outline::class_members(*text, found.entry.providerLine);
            if (members) served.provenance.source = found.source;   // the kept answer, brought up to the class as it is
        }
        auto composed = answer::compose_completion(found.entry, found.source, found.certainty, asked.context, asked.text, asked.position, settings_.marks,
                                                   members, found.providerChanged);
        served.result = std::move(composed.result);
        served.doubted = composed.doubted;
        break;
    }
    case answer::Kind::signature_help: served.result = answer::compose_signature(found.entry, asked.context).result; break;
    case answer::Kind::hover:
        served.result = answer::compose_hover(found.entry, found.source, found.certainty, asked.context, asked.text, settings_.marks).result;
        break;
    default: {
        std::optional<Json> relocated;
        if (found.certainty != answer::Certainty::certain) {
            // The file it lands in changed: the declaration found again in its text, when it is there once.
            if (const auto target = first_location(found.entry.result)) {
                const auto& [path, line, column] = *target;
                std::optional<std::string> text { text_of_(path) };
                if (!text) {
                    if (auto disk = os::fs::read(path)) text = std::move(*disk);
                }
                if (text) {
                    const auto places = outline::declarations(*text, asked.context.word);
                    if (places.size() == 1) {
                        const base::Position start { places[0].line, static_cast<int>(base::utf16_length(base::split_lines(*text)[static_cast<std::size_t>(places[0].line)].substr(0, static_cast<std::size_t>(places[0].column)))) };
                        const base::Position end { start.line, start.character + static_cast<int>(base::utf16_length(asked.context.word)) };
                        relocated = Json::array({ Json { { "uri", base::path_to_uri(path) },
                                                         { "range", Json { { "start", Json { { "line", start.line }, { "character", start.character } } },
                                                                           { "end", Json { { "line", end.line }, { "character", end.character } } } } } } });
                        served.provenance.source = answer::Source::outline;
                        (void)line;
                        (void)column;
                    }
                }
            }
        }
        auto jump = answer::compose_jump(found.entry, found.certainty, relocated);
        if (!jump) return std::nullopt;
        served.result = std::move(*jump);
    }
    }
    served.provenance.note = answer::note_for(asked.kind, served.provenance);
    return served;
}

void Layer::keep(const Asked& asked, const Json& result, const std::optional<Served>& served, std::chrono::milliseconds took) {
    if (served && asked.kind == answer::Kind::completion) {
        const auto [gone, added] = answer::difference(served->result, result);
        if (gone == 0 && added == 0) ++counts_.replacedSame;
        else ++counts_.replacedDifferent;
        counts_.itemsGone += gone;
        counts_.itemsAdded += added;
        if (gone + added > 0) {
            log::info("answer {} {}:{}:{} {} source=engine certainty=authoritative took={}ms replaced={} diff=-{}/+{}", answer::name_of(asked.kind),
                      base::file_name(asked.path), asked.position.line + 1, asked.position.character + 1, answer::name_of(asked.context.place), took.count(),
                      answer::name_of(served->provenance.source), gone, added);
        }
    }
    auto trimmed = answer::trim(asked.kind, result);
    if (!trimmed) return;
    const answer::Now now { now_(asked) };
    answer::Entry entry;
    entry.kind = asked.kind;
    entry.result = std::move(*trimmed);
    entry.fingerprint = asked.fingerprint;
    entry.others = now.others;
    entry.localHash = asked.context.localHash;
    entry.asked = asked.uri;
    if (asked.kind == answer::Kind::completion) {
        entry.word = asked.context.word;
        entry.complete = result.is_array() || (result.is_object() && !result.value("isIncomplete", false));   // CompletionItem[] is complete
    }
    entry.keptAt = now_ms();
    const Type* type { nullptr };
    if (const auto it = placeTypes_.find(asked.k1); it != placeTypes_.end()) type = &it->second;
    else if (asked.declaredType) {
        if (const auto spelled = spelledTypes_.find(*asked.declaredType); spelled != spelledTypes_.end()) type = &spelled->second;
    }
    if (answer::is_jump(asked.kind)) {
        if (const auto target = first_location(result)) {
            entry.provider = std::get<0>(*target);
            entry.providerLine = std::get<1>(*target);
            entry.providerGeneration = generation_(entry.provider);
        }
    } else if (type != nullptr) {
        entry.provider = type->path;
        const std::size_t colon { type->key.rfind(':') };
        entry.providerLine = colon == std::string::npos ? -1 : std::atoi(type->key.c_str() + colon + 1);
        entry.providerGeneration = generation_(type->path);
    }
    if (asked.k3) store_.put(*asked.k3, entry);
    if (type != nullptr && (asked.kind == answer::Kind::completion || asked.kind == answer::Kind::hover)) {
        store_.put(answer::k2(asked.kind, asked.context.place, type->key), entry);
    }
    store_.put(asked.k1, std::move(entry));
}

void Layer::learn_type(const Asked& asked, std::string_view name, std::string_view path, int line) {
    if (name.empty() || path.empty()) return;
    Type type { answer::type_key(name, path, line), std::string { path }, generation_(path) };
    if (asked.declaredType) spelledTypes_[*asked.declaredType] = type;
    placeTypes_[asked.k1] = type;
    if (placeTypes_.size() > 20000) placeTypes_.clear();
    if (spelledTypes_.size() > 4000) spelledTypes_.clear();
    ++counts_.background["learned"];
    // The answer just kept for the place is the type's answer too.
    if (const answer::Entry* kept = store_.find(asked.k1)) {
        answer::Entry entry { *kept };
        entry.provider = std::string { path };
        entry.providerLine = line;
        entry.providerGeneration = type.generation;
        store_.put(answer::k2(asked.kind, asked.context.place, type.key), entry);
        store_.put(asked.k1, std::move(entry));
    }
}

bool Layer::knows_type(const Asked& asked) const {
    return placeTypes_.contains(asked.k1) || (asked.declaredType && spelledTypes_.contains(*asked.declaredType));
}

namespace {

// Whether a completion context is inside a function's body: its innermost named declaration is a function, not a
// namespace, class or enum (answer::enclosing_of names those by their keyword).
bool in_function_body(const answer::Context& context) {
    if (context.enclosing.empty()) return false;
    for (std::string_view key : { "namespace ", "class ", "struct ", "union ", "enum " }) {
        if (context.enclosing.starts_with(key)) return false;
    }
    return true;
}

} // namespace

std::optional<base::Position> Layer::prefetch_place(std::string_view uri, std::string_view text, base::Position near, std::string_view fingerprint) {
    // The triggers the file has, nearest first, within a screenful or two of where the person is.
    struct Candidate {
        int distance;
        std::size_t offset;
    };
    std::vector<Candidate> candidates;
    int line { 0 };
    for (std::size_t i { 0 }; i + 1 < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
            continue;
        }
        const int distance { std::abs(line - near.line) };
        if (distance > 150) {
            if (line > near.line) break;
            continue;
        }
        std::size_t after { std::string_view::npos };
        if (text[i] == '.' && (i == 0 || text[i - 1] != '.') && (std::isalpha(static_cast<unsigned char>(text[i + 1])) != 0 || text[i + 1] == '_')) after = i + 1;
        else if (text[i] == '-' && text[i + 1] == '>' && i + 2 < text.size() && (std::isalpha(static_cast<unsigned char>(text[i + 2])) != 0 || text[i + 2] == '_')) after = i + 2;
        else if (text[i] == ':' && text[i + 1] == ':' && i + 2 < text.size() && (std::isalpha(static_cast<unsigned char>(text[i + 2])) != 0 || text[i + 2] == '_')) after = i + 2;
        if (after != std::string_view::npos) candidates.push_back(Candidate { distance, after });
        if (candidates.size() >= 400) break;
    }
    std::ranges::stable_sort(candidates, {}, &Candidate::distance);
    for (const Candidate& candidate : candidates | std::views::take(200)) {
        const base::Position position { base::position_at(text, candidate.offset) };
        const auto context = answer::context_at(text, position, answer::Kind::completion);
        if (!context || context->place == answer::Context::Place::other || context->place == answer::Context::Place::unqualified) continue;
        // Only in a function's body, where a person types `.`, `->` and `::`: a qualified name in a declaration's head
        // (`namespace a::b {`, a base class, a return type) is asked of the engine by nobody, and is where its parser is
        // least tried.
        if (!in_function_body(*context)) continue;
        const std::size_t newline { candidate.offset == 0 ? std::string_view::npos : text.rfind('\n', candidate.offset - 1) };
        const std::size_t lineStart { newline == std::string_view::npos ? 0 : newline + 1 };
        const std::string_view lineText { base::trim(text.substr(lineStart, candidate.offset - lineStart)) };
        if (lineText.starts_with('#') || lineText.starts_with("//") || lineText.starts_with('*') || lineText.starts_with("/*")) continue;
        const std::string key { answer::k1(uri, answer::Kind::completion, *context) };
        if (store_.contains(key)) continue;
        if (const auto empty = fruitless_.find(key); empty != fruitless_.end() && empty->second == std::format("{}|{}", fingerprint, total_)) continue;
        return position;
    }
    return std::nullopt;
}

void Layer::prefetch_came_back_empty(const Asked& asked) {
    // Any change since (`total_` counts every file's, and a new plan) asks it again.
    if (fruitless_.size() > 4096) fruitless_.clear();
    fruitless_[asked.k1] = std::format("{}|{}", asked.fingerprint, total_);
}

void Layer::load() {
    if (!enabled() || settings_.cache != "on" || workspaceFile_.empty()) return;
    const auto started = Clock::now();
    auto text = os::fs::read(workspaceFile_);
    if (!text) return;
    Json persisted = Json::parse(*text, nullptr, false);
    if (persisted.is_discarded() || !persisted.is_object() || persisted.value("format", 0) != FILE_FORMAT || !store_.adopt(persisted.value("store", Json()))) {
        log::info("answers: {} is not one this version reads; starting without it", workspaceFile_);
        (void)[&] { std::error_code failed; std::filesystem::remove(workspaceFile_, failed); return !failed; }();
        return;
    }
    const auto adoptTypes = [&](const char* name, std::unordered_map<std::string, Type>& into) {
        const Json types = persisted.value(name, Json::object());
        if (!types.is_object()) return;
        for (auto it = types.begin(); it != types.end(); ++it) {
            if (!it.value().is_object()) continue;
            Type type { it.value().value("key", std::string {}), it.value().value("path", std::string {}), 0 };
            type.generation = generation_(type.path);
            into[it.key()] = std::move(type);
        }
    };
    adoptTypes("placeTypes", placeTypes_);
    adoptTypes("spelledTypes", spelledTypes_);
    // Names (K3) every workspace with this toolchain shares.
    if (!namesDirectory_.empty()) {
        for (const auto& file : base::children(namesDirectory_)) {
            if (!file.ends_with(".json")) continue;
            if (auto names = os::fs::read(base::join_path(namesDirectory_, file))) {
                const Json shared = Json::parse(*names, nullptr, false);
                if (!shared.is_discarded()) store_.adopt(shared);
            }
        }
    }
    savedAt_ = store_.changes();
    log::info("answers: {} kept from the last session ({} KB), read in {} ms", store_.size(), store_.bytes() / 1024,
              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

void Layer::save(bool force) {
    std::error_code ignored;
    if (!enabled() || settings_.cache != "on" || workspaceFile_.empty()) return;
    if (store_.changes() == savedAt_) return;
    if (!force && lastSave_ && Clock::now() - *lastSave_ < SAVE_EVERY) return;
    lastSave_ = Clock::now();
    savedAt_ = store_.changes();
    // The most recently used, within what a file keeps.
    Json all = store_.to_json();
    Json& entries = all["entries"];
    std::size_t bytes { 0 };
    std::size_t keep { 0 };
    for (std::size_t i { entries.size() }; i > 0; --i) {
        bytes += entries[i - 1].dump().size();
        if (bytes > PERSISTED_BYTES) break;
        ++keep;
    }
    if (keep < entries.size()) entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(entries.size() - keep));
    const auto typesJson = [](const std::unordered_map<std::string, Type>& types) {
        Json out = Json::object();
        for (const auto& [key, type] : types) out[key] = Json { { "key", type.key }, { "path", type.path } };
        return out;
    };
    const Json persisted { { "format", FILE_FORMAT }, { "store", std::move(all) }, { "placeTypes", typesJson(placeTypes_) }, { "spelledTypes", typesJson(spelledTypes_) } };
    (void)std::filesystem::create_directories(base::parent_path(workspaceFile_), ignored);
    if (auto written = os::fs::write_atomic(workspaceFile_, persisted.dump()); !written) {
        log::warning("answers: could not keep them in {}: {}", workspaceFile_, written.error().message());
    }
    // The names, for every workspace: this one's, beside what the file has from others.
    if (!namesDirectory_.empty()) {
        Json names = store_.to_json(true);
        if (names["entries"].empty()) return;
        const std::string file { base::join_path(namesDirectory_, "names.json") };
        answer::Store merged { PERSISTED_BYTES };
        if (auto text = os::fs::read(file)) {
            const Json existing = Json::parse(*text, nullptr, false);
            if (!existing.is_discarded()) merged.adopt(existing);
        }
        for (const Json& one : names["entries"]) merged.erase(one.value("key", std::string {}));
        merged.adopt(names);
        (void)std::filesystem::create_directories(namesDirectory_, ignored);
        (void)os::fs::write_atomic(file, merged.to_json(true).dump());
    }
}

void Layer::count_served(const Asked& asked, const Served& served, std::chrono::milliseconds took) {
    ++counts_.bySource[std::format("{} {}", answer::name_of(asked.kind), answer::name_of(served.provenance.source))];
    push_ms(counts_.servedMs, took);
    counts_.doubted += served.doubted;
}

void Layer::count_engine(const Asked& asked, std::chrono::milliseconds took) {
    ++counts_.bySource[std::format("{} engine", answer::name_of(asked.kind))];
    push_ms(counts_.engineMs, took);
}

void Layer::count_background(std::string_view what) { ++counts_.background[std::string { what }]; }

Json Layer::report() const {
    return Json { { "cache", settings_.cache },
                  { "budgetMs", settings_.budget.count() },
                  { "entries", store_.size() },
                  { "kilobytes", store_.bytes() / 1024 },
                  { "types", placeTypes_.size() + spelledTypes_.size() },
                  { "answers", counts_.bySource },
                  { "fromCache", percentiles(counts_.servedMs) },
                  { "fromEngine", percentiles(counts_.engineMs) },
                  { "replaced", Json { { "same", counts_.replacedSame }, { "different", counts_.replacedDifferent }, { "itemsGone", counts_.itemsGone },
                                       { "itemsAdded", counts_.itemsAdded } } },
                  { "itemsInDoubt", counts_.doubted },
                  { "background", counts_.background } };
}

std::string Layer::log_line(const Asked& asked, const Served& served, std::chrono::milliseconds took) {
    return std::format("answer {} {}:{}:{} {} source={} certainty={} age={}s took={}ms{}", answer::name_of(asked.kind), base::file_name(asked.path),
                       asked.position.line + 1, asked.position.character + 1, answer::name_of(asked.context.place),
                       answer::name_of(served.provenance.source), answer::name_of(served.provenance.certainty), served.provenance.ageMs / 1000, took.count(),
                       served.provenance.enginePending ? " engine=pending" : "");
}

} // namespace mcppls::orchestrator::answering
