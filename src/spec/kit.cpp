module mcppls.spec.kit;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.platform.fs;

namespace mcppls::spec {

namespace {

constexpr std::array<std::string_view, 1> KNOWN_REQUIREMENTS { "macos-sdk" };

// Kit paths are relative, '/'-separated, without a drive and without "..".
base::Result<std::string> kit_path(std::string_view root, const nlohmann::json& value, std::string_view field) {
    if (!value.is_string()) return base::fail("kit-invalid", std::format("{} must be a string", field));
    const std::string path { value.get<std::string>() };
    if (path.empty() || path.find('\\') != std::string::npos || path.starts_with('/')
        || (path.size() >= 2 && path[1] == ':')) {
        return base::fail("kit-invalid", std::format("{} must be a relative '/' path: {}", field, path));
    }
    for (auto part : std::views::split(std::string_view { path }, '/')) {
        if (std::string_view { part.begin(), part.end() } == "..") {
            return base::fail("kit-invalid", std::format("{} must stay inside the kit: {}", field, path));
        }
    }
    return base::join_path(root, path);
}

} // namespace

base::Result<Kit> parse_kit(const nlohmann::json& document, std::string_view kitRoot) {
    if (!document.is_object()) return base::fail("kit-invalid", "kit.json is not an object");
    if (document.value("kit-version", 0) != KIT_VERSION) {
        return base::fail("kit-unsupported", std::format("unsupported kit-version {}", document.value("kit-version", 0)));
    }
    Kit kit;
    kit.root = base::normalize_path(kitRoot);
    kit.name = document.value("name", std::string {});
    kit.target = document.value("target", std::string {});
    if (kit.name.empty() || kit.target.empty()) return base::fail("kit-invalid", "kit.json needs name and target");

    const auto stdlib = document.find("stdlib");
    if (stdlib == document.end() || !stdlib->is_object()) return base::fail("kit-invalid", "kit.json needs stdlib");
    kit.stdlibName = stdlib->value("name", std::string {});
    kit.stdlibVersion = stdlib->value("version", std::string {});
    const auto metadata = stdlib->find("module-metadata");
    if (kit.stdlibName.empty() || kit.stdlibVersion.empty() || metadata == stdlib->end()) {
        return base::fail("kit-invalid", "stdlib needs name, version and module-metadata");
    }
    auto metadataPath = kit_path(kit.root, *metadata, "stdlib.module-metadata");
    if (!metadataPath) return std::unexpected { metadataPath.error() };
    kit.moduleMetadata = std::move(*metadataPath);

    const auto includes = document.find("system-include-directories");
    if (includes == document.end() || !includes->is_array()) return base::fail("kit-invalid", "kit.json needs system-include-directories");
    for (const auto& directory : *includes) {
        auto path = kit_path(kit.root, directory, "system-include-directories");
        if (!path) return std::unexpected { path.error() };
        kit.systemIncludeDirectories.push_back(std::move(*path));
    }
    if (const auto sysroot = document.find("sysroot"); sysroot != document.end() && !sysroot->is_null()) {
        auto path = kit_path(kit.root, *sysroot, "sysroot");
        if (!path) return std::unexpected { path.error() };
        kit.sysroot = std::move(*path);
    }
    for (const auto& argument : document.value("arguments", nlohmann::json::array())) {
        if (argument.is_string()) kit.arguments.push_back(argument.get<std::string>());
    }
    for (const auto& requirement : document.value("requires", nlohmann::json::array())) {
        const std::string kind { requirement.is_object() ? requirement.value("kind", std::string {}) : std::string {} };
        if (std::ranges::find(KNOWN_REQUIREMENTS, kind) == KNOWN_REQUIREMENTS.end()) {
            return base::fail("kit-unsupported", std::format("unknown kit requirement \"{}\"", kind));
        }
        kit.requirements.push_back(kind);
    }
    for (const auto& license : document.value("licenses", nlohmann::json::array())) {
        auto path = kit_path(kit.root, license, "licenses");
        if (!path) return std::unexpected { path.error() };
        kit.licenses.push_back(std::move(*path));
    }
    return kit;
}

base::Result<Kit> load_kit(std::string_view kitRoot) {
    const std::string manifest { base::join_path(kitRoot, "kit.json") };
    auto text = platform::fs::read_file(manifest);
    if (!text) return base::fail("kit-missing", std::format("no kit.json in {}", kitRoot));
    nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
    if (document.is_discarded()) return base::fail("kit-invalid", std::format("{} is not valid JSON", manifest));
    return parse_kit(document, kitRoot);
}

bool requires_macos_sdk(const Kit& kit) {
    return std::ranges::find(kit.requirements, "macos-sdk") != kit.requirements.end();
}

} // namespace mcppls::spec
