// S4: the semantic kit manifest (docs/specs/s4-semantic-kit.md). A kit carries a
// standard library's headers and module sources without a compiler. Kit version 2 lets one kit
// serve several targets (`targets`): the openkal kit a payload carries holds one set of headers
// and the generated headers of each architecture.
export module mcppls.spec.kit;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::spec {

// The highest kit-version this consumer reads; every version from 1 up to it is read.
inline constexpr int KIT_VERSION { 2 };

// One entry of kit-version 2's `targets`: the target a kit serves besides (or as) its top-level one,
// keyed by the target as mcpp spells it (`x86_64-openkal`), with the triple the engine is given.
struct KitVariant {
    std::string name;                                    // the key: `x86_64-openkal`
    std::string target;                                  // the engine's --target: `x86_64-unknown-none-elf`
    std::vector<std::string> systemIncludeDirectories;   // absolute, in order
    std::optional<std::string> sysroot;                  // absolute
    std::vector<std::string> arguments;
};

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
    std::vector<KitVariant> variants;                    // kit-version 2 `targets`, in name order; empty for a kit of one target
};

base::Result<Kit> parse_kit(const nlohmann::json& document, std::string_view kitRoot);
base::Result<Kit> load_kit(std::string_view kitRoot);
bool requires_macos_sdk(const Kit& kit);
// The kit as it serves the variant `name` (`targets.<name>`): its target, include directories,
// sysroot and arguments in place of the top-level ones. Nullopt when the kit has no such variant.
std::optional<Kit> kit_variant(const Kit& kit, std::string_view name);

} // namespace mcppls::spec
