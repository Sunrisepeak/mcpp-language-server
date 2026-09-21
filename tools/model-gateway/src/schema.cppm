// PROTOCOL.md's schema check for `complete`: not full JSON Schema, only the
// schema's top-level `required` keys and top-level `properties` `type`s.
export module mcppls.model.schema;

import std;
import nlohmann.json;

export namespace mcppls::model {

bool matches_schema(const nlohmann::json& schema, const nlohmann::json& value);

} // namespace mcppls::model
