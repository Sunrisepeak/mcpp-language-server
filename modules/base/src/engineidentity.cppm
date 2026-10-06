// Immutable engine provenance shared by assembly and runtime resolution.
export module mcppls.base.engineidentity;

import std;
import mcppls.base.error;
import mcppls.base.version;

export namespace mcppls::base {

struct EngineIdentity {
    std::string version;
    std::string llvmBase;
    std::string llvmCommit;
    std::string forkCommit;
    std::string patchDigest;
    std::string platform;
    std::string sha256;
    std::vector<std::string> features;
};

inline std::optional<std::string> engine_identity_problem(const EngineIdentity& identity,
                                                         std::string_view version,
                                                         std::string_view platform,
                                                         std::string_view digest) {
    const auto hex = [](std::string_view value, std::size_t length) {
        return value.size() == length && value.find_first_not_of('0') != std::string_view::npos && std::ranges::all_of(value, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    if (identity.version != version || identity.platform != platform) return "engine version or platform identity differs";
    const auto validBase = [](std::string_view value) {
        return !value.empty() && value.front() != '.' && value.back() != '.' &&
               value.find("..") == std::string_view::npos && std::ranges::count(value, '.') == 2 &&
               std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
    };
    if (!validBase(identity.llvmBase) || identity.llvmBase != llvm_base_version(version)) return "LLVM base version identity differs";
    if (!hex(identity.llvmCommit, 40) || !hex(identity.forkCommit, 40) || !hex(identity.patchDigest, 64)) return "engine source identity is missing or malformed";
    if (!hex(identity.sha256, 64) || identity.sha256 != digest) return "engine binary SHA identity differs";
    std::set<std::string> features;
    for (const auto& feature : identity.features) {
        if (feature.empty() || !features.insert(feature).second) return "engine features are empty or duplicated";
    }
    return std::nullopt;
}

// A JSON-like object is accepted without coupling the base package to a JSON
// implementation. All fields are type checked before their values are read.
template<class JsonObject>
Result<EngineIdentity> decode_engine_identity(const JsonObject& object) {
    if (!object.is_object()) return fail("engine-identity", "engine identity is not an object");
    EngineIdentity identity;
    for (const auto& [name, destination] : std::array<std::pair<std::string_view, std::string*>, 7> {
             std::pair { "engine-version", &identity.version }, { "llvm-base-version", &identity.llvmBase },
             { "llvm-commit", &identity.llvmCommit }, { "fork-commit", &identity.forkCommit },
             { "patch-series-sha256", &identity.patchDigest }, { "platform", &identity.platform }, { "sha256", &identity.sha256 } }) {
        const auto value = object.find(name);
        if (value == object.end() || !value->is_string()) return fail("engine-identity", std::format("engine identity lacks string {}", name));
        *destination = value->template get<std::string>();
    }
    const auto features = object.find("features");
    if (features == object.end() || !features->is_array()) return fail("engine-identity", "engine identity lacks a features array");
    for (const auto& feature : *features) {
        if (!feature.is_string()) return fail("engine-identity", "engine feature is not a string");
        identity.features.push_back(feature.template get<std::string>());
    }
    return identity;
}

} // namespace mcppls::base
