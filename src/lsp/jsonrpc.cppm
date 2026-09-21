// JSON-RPC 2.0 over the Language Server Protocol base protocol: framing with
// Content-Length headers, message classification and constructors.
export module mcppls.lsp.jsonrpc;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::lsp {

using Json = nlohmann::json;

class FrameReader {
private:
    std::string buffer_;
    std::size_t maxMessageBytes_ { 256u * 1024u * 1024u };

public:
    void feed(std::string_view bytes);
    // The next complete message; nullopt when more bytes are needed. A malformed
    // frame yields an error and is skipped, so reading can continue.
    std::optional<base::Result<Json>> next();
    std::size_t buffered() const { return buffer_.size(); }
};

std::string encode_frame(const Json& message);
// Serializes without throwing: invalid UTF-8 is replaced, never an exception.
std::string dump(const Json& value);
// Parses without throwing.
base::Result<Json> parse(std::string_view text);

enum class Kind { request, notification, response, invalid };
Kind kind_of(const Json& message);

Json make_request(const Json& id, std::string_view method, Json params);
Json make_notification(std::string_view method, Json params);
Json make_result(const Json& id, Json result);
Json make_error(const Json& id, int code, std::string_view message, Json data = nullptr);

// JSON-RPC and LSP error codes used by the server.
inline constexpr int PARSE_ERROR { -32700 };
inline constexpr int INVALID_REQUEST { -32600 };
inline constexpr int METHOD_NOT_FOUND { -32601 };
inline constexpr int INVALID_PARAMS { -32602 };
inline constexpr int INTERNAL_ERROR { -32603 };
inline constexpr int SERVER_NOT_INITIALIZED { -32002 };
inline constexpr int REQUEST_FAILED { -32803 };
inline constexpr int SERVER_CANCELLED { -32802 };
inline constexpr int CONTENT_MODIFIED { -32801 };
inline constexpr int REQUEST_CANCELLED { -32800 };

// Small accessors that never throw.
std::optional<std::string> string_at(const Json& object, std::string_view key);
std::optional<std::int64_t> int_at(const Json& object, std::string_view key);
const Json* find(const Json& object, std::string_view key);
// Walks object keys; nullptr when any step is missing.
const Json* find_path(const Json& object, std::initializer_list<std::string_view> keys);

} // namespace mcppls::lsp
