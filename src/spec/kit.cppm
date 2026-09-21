// S4: the semantic kit manifest (docs/specs/s4-semantic-kit.md). A kit carries a
// standard library's headers and module sources without a compiler.
export module mcppls.spec.kit;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::spec {

inline constexpr int KIT_VERSION { 1 };

struct Kit {
    std::string root;                                    // absolute kit directory
    std::string name;
    std::string target;
    std::string stdlibName;
    std::string stdlibVersion;
    std::string moduleMetadata;                          // absolute
    std::vector<std::string> systemIncludeDirectories;   // absolute, in order
    std::optional<std::string> sysroot;                  // absolute
    std::vector<std::string> arguments;
    std::vector<std::string> requirements;               // kinds, e.g. "macos-sdk"
    std::vector<std::string> licenses;                   // absolute
};

base::Result<Kit> parse_kit(const nlohmann::json& document, std::string_view kitRoot);
base::Result<Kit> load_kit(std::string_view kitRoot);
bool requires_macos_sdk(const Kit& kit);

} // namespace mcppls::spec
