// The wire envelope for PROTOCOL.md's line protocol: one JSON object per
// line, request/response correlated by `id`, notifications carry none. See
// PROTOCOL.md for the full contract this implements.
export module mcppls.model.protocol;

import std;
import nlohmann.json;

export namespace mcppls::model {

// JSON-RPC-style codes plus this protocol's own range (PROTOCOL.md "Error codes").
inline constexpr int PARSE_ERROR { -32700 };
inline constexpr int METHOD_NOT_FOUND { -32601 };
inline constexpr int INVALID_PARAMS { -32602 };
inline constexpr int PROVIDER_ERROR { 1001 };
inline constexpr int NOT_CONFIGURED { 1002 };
inline constexpr int CANCELLED { 1003 };
inline constexpr int SCHEMA_MISMATCH { 1004 };

struct RpcError {
    int code;
    std::string message;
};

// `id` is echoed back exactly as received (PROTOCOL.md: response carries "the
// same" id); pass a JSON null for a parse error, whose request id is unknown.
nlohmann::json make_result(const nlohmann::json& id, nlohmann::json result);
nlohmann::json make_error(const nlohmann::json& id, const RpcError& error);
nlohmann::json make_error(const nlohmann::json& id, int code, std::string message);

} // namespace mcppls::model
