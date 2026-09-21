// Module metadata manifests: the P3286 / EcoStd shape of libc++.modules.json and
// libstdc++.modules.json, and the MSVC STL's modules.json (module-sources).
export module mcppls.spec.metadata;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::spec {

struct ModuleDefinition {
    std::string name;
    std::optional<std::string> value;
};

struct ModuleEntry {
    std::string logicalName;
    std::string source;                                  // absolute
    bool isStdLibrary { false };
    std::vector<std::string> systemIncludeDirectories;   // absolute
    std::vector<ModuleDefinition> definitions;
};

// Relative paths in the manifest resolve against the manifest's directory.
base::Result<std::vector<ModuleEntry>> parse_module_metadata(const nlohmann::json& document, std::string_view manifestDirectory);
base::Result<std::vector<ModuleEntry>> read_module_metadata(std::string_view manifestPath);

} // namespace mcppls::spec
