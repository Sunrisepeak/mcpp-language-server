module mcppls.orchestrator.client;

import std;
import nlohmann.json;
import mcppls.base.log;
import mcppls.platform.stdio;
import mcppls.lsp.jsonrpc;

namespace mcppls::orchestrator {

void ClientSink::reply(const Json& id, Json result) { send(lsp::make_result(id, std::move(result))); }

void ClientSink::reply_error(const Json& id, int code, std::string_view message) { send(lsp::make_error(id, code, message)); }

void ClientSink::notify(std::string_view method, Json params) { send(lsp::make_notification(method, std::move(params))); }

namespace {
std::mutex gWriteStatsMutex;
WriteStats gWriteStats;
} // namespace

void StdioSink::send(const Json& message) {
    const std::string frame { lsp::encode_frame(message) };
    const auto started = std::chrono::steady_clock::now();
    const auto written = platform::stdio::write_output(frame);
    const auto ms = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    {
        const std::lock_guard lock { gWriteStatsMutex };
        ++gWriteStats.writes;
        gWriteStats.bytes += frame.size();
        gWriteStats.totalMs += ms;
        gWriteStats.maxMs = std::max(gWriteStats.maxMs, ms);
        if (ms >= 100) ++gWriteStats.slowWrites;
    }
    if (!written) base::log::error("cannot write to the client: {}", written.error().message);
}

WriteStats stdio_write_stats() {
    const std::lock_guard lock { gWriteStatsMutex };
    return gWriteStats;
}

namespace {

// A length-prefixed field: a root's path is exactly the kind of text that contains ':', and every
// Windows path does right after its drive letter.
void append_field(std::string& key, std::string_view field) { key += std::format("{}:{}:", field.size(), field); }

std::optional<std::string_view> take_field(std::string_view& rest) {
    const std::size_t lengthEnd { rest.find(':') };
    if (lengthEnd == std::string_view::npos) return std::nullopt;
    std::size_t length { 0 };
    try {
        length = static_cast<std::size_t>(std::stoull(std::string { rest.substr(0, lengthEnd) }));
    } catch (...) {
        return std::nullopt;
    }
    rest = rest.substr(lengthEnd + 1);
    if (rest.size() < length + 1 || rest[length] != ':') return std::nullopt;
    const std::string_view field { rest.substr(0, length) };
    rest = rest.substr(length + 1);
    return field;
}

} // namespace

std::string make_engine_request_key(std::string_view rootKey, std::string_view engineId, int generation, const Json& engineRequestId) {
    std::string key { "e:" };
    append_field(key, rootKey);
    append_field(key, engineId);
    key += std::format("{}:{}", generation, lsp::dump(engineRequestId));
    return key;
}

std::optional<EngineRequestKey> parse_engine_request_key(std::string_view key) {
    if (!key.starts_with("e:")) return std::nullopt;
    std::string_view rest { key.substr(2) };
    const auto rootKey = take_field(rest);
    if (!rootKey) return std::nullopt;
    const auto engineId = take_field(rest);
    if (!engineId) return std::nullopt;
    const std::size_t generationEnd { rest.find(':') };
    if (generationEnd == std::string_view::npos) return std::nullopt;
    int generation { 0 };
    try {
        generation = std::stoi(std::string { rest.substr(0, generationEnd) });
    } catch (...) {
        return std::nullopt;
    }
    // Not `Json id { ... }`: brace-initializing wraps a scalar (the plain integer ids clangd uses) in an array.
    Json id = Json::parse(rest.substr(generationEnd + 1), nullptr, false);
    if (id.is_discarded()) return std::nullopt;
    return EngineRequestKey { std::string { *rootKey }, std::string { *engineId }, generation, std::move(id) };
}

} // namespace mcppls::orchestrator
