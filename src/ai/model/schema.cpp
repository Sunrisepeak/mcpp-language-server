module mcppls.ai.model.schema;

import std;
import nlohmann.json;

namespace mcppls::ai::model {

namespace {

using Json = nlohmann::json;

// RFC 6901: '~' and '/' are the two characters a pointer token must escape.
std::string escape_pointer(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (char c : token) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

// JSON Schema measures string length in Unicode characters, not UTF-8 bytes.
std::size_t codepoint_length(std::string_view utf8) {
    std::size_t count { 0 };
    for (unsigned char c : utf8) {
        if ((c & 0xC0) != 0x80) ++count;   // not a continuation byte: a new codepoint starts here
    }
    return count;
}

std::string_view type_name(const Json& value) {
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    if (value.is_string()) return "string";
    if (value.is_boolean()) return "boolean";
    if (value.is_null()) return "null";
    if (value.is_number()) return "number";
    return "unknown";
}

bool matches_type(std::string_view type, const Json& value) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    if (type == "number") return value.is_number();
    if (type == "integer") {
        if (value.is_number_integer()) return true;
        return value.is_number_float() && std::floor(value.get<double>()) == value.get<double>();
    }
    return false;
}

std::vector<std::string> allowed_types(const Json& schema) {
    std::vector<std::string> types;
    const auto found = schema.find("type");
    if (found == schema.end()) return types;
    if (found->is_string()) {
        types.push_back(found->get<std::string>());
    } else if (found->is_array()) {
        for (const auto& entry : *found) {
            if (entry.is_string()) types.push_back(entry.get<std::string>());
        }
    }
    return types;
}

std::string joined(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i { 0 }; i < values.size(); ++i) {
        if (i) out += " | ";
        out += values[i];
    }
    return out;
}

void validate_node(const Json& schema, const Json& value, const std::string& path, std::vector<SchemaError>& errors) {
    if (!schema.is_object()) return;   // an empty/non-object subschema constrains nothing here

    if (const auto found = schema.find("enum"); found != schema.end() && found->is_array()) {
        const bool matched { std::ranges::any_of(*found, [&](const Json& candidate) { return candidate == value; }) };
        if (!matched) errors.push_back({ path, "value is not one of the schema's enum values" });
    }

    const std::vector<std::string> types { allowed_types(schema) };
    if (!types.empty() && !std::ranges::any_of(types, [&](const std::string& t) { return matches_type(t, value); })) {
        errors.push_back({ path, std::format("expected type {}, got {}", joined(types), type_name(value)) });
        return;   // properties/items/range checks below would only produce misleading noise
    }

    if (value.is_object()) {
        if (const auto required = schema.find("required"); required != schema.end() && required->is_array()) {
            for (const auto& name : *required) {
                if (name.is_string() && !value.contains(name.get<std::string>())) {
                    errors.push_back({ path + "/" + escape_pointer(name.get<std::string>()), "required property is missing" });
                }
            }
        }
        const Json* properties { nullptr };
        if (const auto found = schema.find("properties"); found != schema.end() && found->is_object()) properties = &*found;
        bool rejectAdditional { false };
        if (const auto found = schema.find("additionalProperties"); found != schema.end() && found->is_boolean() && !found->get<bool>()) {
            rejectAdditional = true;
        }
        // Not structured bindings: nlohmann's iteration_proxy_value only specializes std::tuple_element
        // for its non-const iterator, so `for (const auto& [k, v] : constValue.items())` fails to compile.
        for (const auto& entry : value.items()) {
            const std::string& key { entry.key() };
            const Json* sub { nullptr };
            if (properties) {
                if (const auto found = properties->find(key); found != properties->end()) sub = &*found;
            }
            const std::string childPath { path + "/" + escape_pointer(key) };
            if (sub) validate_node(*sub, entry.value(), childPath, errors);
            else if (rejectAdditional) errors.push_back({ childPath, "additional property is not allowed" });
        }
    } else if (value.is_array()) {
        if (const auto found = schema.find("minItems"); found != schema.end() && found->is_number_integer()) {
            if (static_cast<std::int64_t>(value.size()) < found->get<std::int64_t>()) errors.push_back({ path, "array has fewer items than minItems" });
        }
        if (const auto found = schema.find("maxItems"); found != schema.end() && found->is_number_integer()) {
            if (static_cast<std::int64_t>(value.size()) > found->get<std::int64_t>()) errors.push_back({ path, "array has more items than maxItems" });
        }
        if (const auto found = schema.find("items"); found != schema.end() && found->is_object()) {
            for (std::size_t i { 0 }; i < value.size(); ++i) validate_node(*found, value[i], path + "/" + std::to_string(i), errors);
        }
    } else if (value.is_string()) {
        const std::size_t length { codepoint_length(value.get<std::string>()) };
        if (const auto found = schema.find("minLength"); found != schema.end() && found->is_number_integer() && found->get<std::int64_t>() >= 0) {
            if (length < static_cast<std::size_t>(found->get<std::int64_t>())) errors.push_back({ path, "string is shorter than minLength" });
        }
        if (const auto found = schema.find("maxLength"); found != schema.end() && found->is_number_integer() && found->get<std::int64_t>() >= 0) {
            if (length > static_cast<std::size_t>(found->get<std::int64_t>())) errors.push_back({ path, "string is longer than maxLength" });
        }
    } else if (value.is_number()) {
        const double number { value.get<double>() };
        if (const auto found = schema.find("minimum"); found != schema.end() && found->is_number() && number < found->get<double>()) {
            errors.push_back({ path, "number is less than minimum" });
        }
        if (const auto found = schema.find("maximum"); found != schema.end() && found->is_number() && number > found->get<double>()) {
            errors.push_back({ path, "number is greater than maximum" });
        }
    }
}

} // namespace

std::vector<SchemaError> validate(const Json& schema, const Json& value) {
    std::vector<SchemaError> errors;
    validate_node(schema, value, "", errors);
    return errors;
}

} // namespace mcppls::ai::model
