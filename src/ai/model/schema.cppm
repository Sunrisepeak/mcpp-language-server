// A validator for the subset of JSON Schema the review output schema (mcppls.ai.model.prompt) needs:
// `type` (a name or an array of names), `properties`, `required`, `additionalProperties: false`,
// `items`, `enum`, `minItems`/`maxItems`, `minimum`/`maximum`, `minLength`/`maxLength`. Not a general
// validator: no `$ref`, no `patternProperties`, no combinators, no formats.
export module mcppls.ai.model.schema;

import std;
import nlohmann.json;

export namespace mcppls::ai::model {

struct SchemaError {
    std::string path;      // JSON Pointer (RFC 6901) to the offending value, "" for the root
    std::string message;
};

std::vector<SchemaError> validate(const nlohmann::json& schema, const nlohmann::json& value);

} // namespace mcppls::ai::model
