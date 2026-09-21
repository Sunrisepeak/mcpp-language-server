module mcppls.model.schema;

import std;
import nlohmann.json;

namespace mcppls::model {

namespace {

using Json = nlohmann::json;

bool type_matches(const Json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "integer") return value.is_number_integer() || value.is_number_unsigned();
    if (type == "number") return value.is_number();
    if (type == "null") return value.is_null();
    return true; // an unrecognized declared type is not this checker's job to enforce
}

// JSON Schema's `type` keyword is a string or an array of alternatives.
bool type_matches_any(const Json& value, const Json& typeField) {
    if (typeField.is_string()) return type_matches(value, typeField.get<std::string>());
    if (typeField.is_array()) {
        for (const auto& alternative : typeField) {
            if (alternative.is_string() && type_matches(value, alternative.get<std::string>())) return true;
        }
        return false;
    }
    return true; // malformed `type`: nothing to check against
}

} // namespace

bool matches_schema(const Json& schema, const Json& value) {
    if (!schema.is_object()) return true; // nothing to check against

    if (auto typeField = schema.find("type"); typeField != schema.end()) {
        if (!type_matches_any(value, *typeField)) return false;
    }

    if (!value.is_object()) return true; // required/properties only apply to an object value

    if (auto required = schema.find("required"); required != schema.end() && required->is_array()) {
        for (const auto& key : *required) {
            if (key.is_string() && !value.contains(key.get<std::string>())) return false;
        }
    }

    if (auto properties = schema.find("properties"); properties != schema.end() && properties->is_object()) {
        for (auto property = properties->begin(); property != properties->end(); ++property) {
            if (!value.contains(property.key())) continue; // presence is `required`'s job, checked above
            if (!property.value().is_object()) continue;
            if (auto propertyType = property.value().find("type"); propertyType != property.value().end()) {
                if (!type_matches_any(value.at(property.key()), *propertyType)) return false;
            }
        }
    }

    return true;
}

} // namespace mcppls::model
