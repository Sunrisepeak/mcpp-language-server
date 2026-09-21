module mcppls.lsp.jsonrpc;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.text;

namespace mcppls::lsp {

void FrameReader::feed(std::string_view bytes) { buffer_.append(bytes); }

std::optional<base::Result<Json>> FrameReader::next() {
    // Headers end at the first blank line; tolerate bare LF from lenient peers.
    std::size_t headerEnd { buffer_.find("\r\n\r\n") };
    std::size_t separatorLength { 4 };
    const std::size_t bareEnd { buffer_.find("\n\n") };
    if (bareEnd != std::string::npos && (headerEnd == std::string::npos || bareEnd < headerEnd)) {
        headerEnd = bareEnd;
        separatorLength = 2;
    }
    if (headerEnd == std::string::npos) {
        if (buffer_.size() > 64 * 1024) {
            buffer_.clear();
            return base::Result<Json> { base::fail("frame-header", "header section too long") };
        }
        return std::nullopt;
    }

    std::optional<std::size_t> contentLength;
    for (auto line : base::split(std::string_view { buffer_ }.substr(0, headerEnd), '\n')) {
        line = base::trim(line);
        if (line.empty()) continue;
        const std::size_t colon { line.find(':') };
        if (colon == std::string_view::npos) continue;
        if (base::iequals_ascii(base::trim(line.substr(0, colon)), "Content-Length")) {
            const std::string_view value { base::trim(line.substr(colon + 1)) };
            std::size_t parsed { 0 };
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error == std::errc {} && end == value.data() + value.size()) contentLength = parsed;
        }
    }
    const std::size_t bodyStart { headerEnd + separatorLength };
    if (!contentLength) {
        buffer_.erase(0, bodyStart);
        return base::Result<Json> { base::fail("frame-header", "missing or invalid Content-Length") };
    }
    if (*contentLength > maxMessageBytes_) {
        buffer_.clear();
        return base::Result<Json> { base::fail("frame-header", "message too large") };
    }
    if (buffer_.size() < bodyStart + *contentLength) return std::nullopt;

    auto message = parse(std::string_view { buffer_ }.substr(bodyStart, *contentLength));
    buffer_.erase(0, bodyStart + *contentLength);
    return message;
}

std::string dump(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string encode_frame(const Json& message) {
    const std::string body { dump(message) };
    return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
}

base::Result<Json> parse(std::string_view text) {
    Json value = Json::parse(text.begin(), text.end(), nullptr, false);
    if (value.is_discarded()) return base::fail("json-parse", "invalid JSON");
    return value;
}

Kind kind_of(const Json& message) {
    if (!message.is_object()) return Kind::invalid;
    const auto method = message.find("method");
    const auto id = message.find("id");
    const bool hasId { id != message.end() && (id->is_number_integer() || id->is_string()) };
    if (method != message.end() && method->is_string()) return hasId ? Kind::request : Kind::notification;
    if (id != message.end() && (message.contains("result") || message.contains("error"))) return Kind::response;
    return Kind::invalid;
}

Json make_request(const Json& id, std::string_view method, Json params) {
    Json message { { "jsonrpc", "2.0" }, { "id", id }, { "method", std::string { method } } };
    if (!params.is_null()) message["params"] = std::move(params);
    return message;
}

Json make_notification(std::string_view method, Json params) {
    Json message { { "jsonrpc", "2.0" }, { "method", std::string { method } } };
    if (!params.is_null()) message["params"] = std::move(params);
    return message;
}

Json make_result(const Json& id, Json result) {
    return Json { { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } };
}

Json make_error(const Json& id, int code, std::string_view message, Json data) {
    Json error { { "code", code }, { "message", std::string { message } } };
    if (!data.is_null()) error["data"] = std::move(data);
    return Json { { "jsonrpc", "2.0" }, { "id", id }, { "error", std::move(error) } };
}

const Json* find(const Json& object, std::string_view key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

const Json* find_path(const Json& object, std::initializer_list<std::string_view> keys) {
    const Json* current { &object };
    for (const auto key : keys) {
        current = find(*current, key);
        if (current == nullptr) return nullptr;
    }
    return current;
}

std::optional<std::string> string_at(const Json& object, std::string_view key) {
    const Json* value { find(object, key) };
    if (value == nullptr || !value->is_string()) return std::nullopt;
    return value->get<std::string>();
}

std::optional<std::int64_t> int_at(const Json& object, std::string_view key) {
    const Json* value { find(object, key) };
    if (value == nullptr || !value->is_number_integer()) return std::nullopt;
    return value->get<std::int64_t>();
}

} // namespace mcppls::lsp
