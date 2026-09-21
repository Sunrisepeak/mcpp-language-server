module mcppls.model.protocol;

import std;
import nlohmann.json;

namespace mcppls::model {

namespace {
using Json = nlohmann::json;
} // namespace

Json make_result(const Json& id, Json result) {
    Json response;
    response["id"] = id;
    response["result"] = std::move(result);
    return response;
}

Json make_error(const Json& id, const RpcError& error) {
    return make_error(id, error.code, error.message);
}

Json make_error(const Json& id, int code, std::string message) {
    Json response;
    response["id"] = id;
    Json errorObject;
    errorObject["code"] = code;
    errorObject["message"] = std::move(message);
    response["error"] = std::move(errorObject);
    return response;
}

} // namespace mcppls::model
