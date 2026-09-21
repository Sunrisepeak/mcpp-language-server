module mcppls.toolchain.visualstudio;

import std;
import nlohmann.json;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;

namespace mcppls::toolchain::visualstudio {

namespace {

// 14.44.35207 -> {14, 44, 35207}; versions that do not parse sort first.
std::vector<long> version_parts(std::string_view version) {
    std::vector<long> parts;
    for (auto part : base::split(version, '.')) {
        long value { 0 };
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (error != std::errc {} || end != part.data() + part.size()) return {};
        parts.push_back(value);
    }
    return parts;
}

bool newer(std::string_view a, std::string_view b) { return version_parts(a) > version_parts(b); }

std::string without_trailing_separator(std::string_view path) {
    std::string value { base::trim(path) };
    while (value.size() > 1 && (value.back() == '\\' || value.back() == '/')) value.pop_back();
    return value;
}

std::string lower_component(std::string_view path) { return base::to_lower_ascii(base::file_name(path)); }

} // namespace

std::string tools_directory_of_cl(std::string_view clPath) {
    const std::string path { base::normalize_path(clPath) };
    if (base::to_lower_ascii(base::file_name(path)) != "cl.exe" && base::to_lower_ascii(base::file_name(path)) != "cl") return {};
    const std::string arch { base::parent_path(path) };
    const std::string host { base::parent_path(arch) };
    const std::string bin { base::parent_path(host) };
    if (lower_component(bin) != "bin" || !lower_component(host).starts_with("host")) return {};
    return base::parent_path(bin);
}

std::string vs_root_of_bundled_clang(std::string_view driverPath) {
    std::string directory { base::parent_path(base::normalize_path(driverPath)) };   // .../bin
    if (lower_component(directory) != "bin") return {};
    directory = base::parent_path(directory);
    if (lower_component(directory) == "x64" || lower_component(directory) == "arm64") directory = base::parent_path(directory);
    if (lower_component(directory) != "llvm") return {};
    const std::string tools { base::parent_path(directory) };
    const std::string vc { base::parent_path(tools) };
    if (lower_component(tools) != "tools" || lower_component(vc) != "vc") return {};
    return base::parent_path(vc);
}

std::optional<Installation> installation_of_tools(std::string_view toolsDirectory) {
    const std::string directory { base::normalize_path(without_trailing_separator(toolsDirectory)) };
    if (directory.empty() || !platform::fs::is_directory(base::join_path(directory, "include"))) return std::nullopt;
    Installation installation;
    installation.toolsDirectory = directory;
    installation.toolsVersion = std::string { base::file_name(directory) };
    installation.hasStdModules = platform::fs::is_regular_file(base::join_path(directory, "modules/std.ixx"));
    // <vsRoot>/VC/Tools/MSVC/<version>
    const std::string msvc { base::parent_path(directory) };
    const std::string tools { base::parent_path(msvc) };
    const std::string vc { base::parent_path(tools) };
    if (lower_component(msvc) == "msvc" && lower_component(tools) == "tools" && lower_component(vc) == "vc") {
        installation.vsRoot = base::parent_path(vc);
    }
    return installation;
}

std::optional<Installation> installation_at(std::string_view vsRoot) {
    std::optional<Installation> best;
    for (const auto& candidate : platform::fs::list_directory(base::join_path(base::normalize_path(vsRoot), "VC/Tools/MSVC"))) {
        auto installation = installation_of_tools(candidate);
        if (!installation) continue;
        if (!best || (installation->hasStdModules && !best->hasStdModules)
            || (installation->hasStdModules == best->hasStdModules && newer(installation->toolsVersion, best->toolsVersion))) {
            best = std::move(installation);
        }
    }
    return best;
}

std::optional<std::string> newest_sdk_version(std::string_view sdkRoot) {
    std::optional<std::string> best;
    const std::string root { base::normalize_path(without_trailing_separator(sdkRoot)) };
    for (const auto& include : platform::fs::list_directory(base::join_path(root, "Include"))) {
        const std::string version { base::file_name(include) };
        if (version_parts(version).empty()) continue;
        if (!platform::fs::is_regular_file(base::join_path(include, "ucrt/corecrt.h"))) continue;
        if (!platform::fs::is_regular_file(base::join_path(root, std::format("Lib/{}/um/x64/kernel32.lib", version)))) continue;
        if (!best || newer(version, *best)) best = version;
    }
    return best;
}

std::vector<std::string> parse_vswhere(std::string_view json) {
    std::vector<std::string> paths;
    const nlohmann::json document = nlohmann::json::parse(json, nullptr, false);
    if (document.is_discarded() || !document.is_array()) return paths;
    for (const auto& instance : document) {
        if (!instance.is_object()) continue;
        if (const auto path = instance.find("installationPath"); path != instance.end() && path->is_string()) {
            paths.push_back(base::normalize_path(path->get<std::string>()));
        }
    }
    return paths;
}

std::string compatibility_version(std::string_view toolsVersion) {
    const auto parts = version_parts(toolsVersion);
    if (parts.size() < 2 || parts[0] != 14) return {};
    return std::format("19.{}", parts[1]);
}

std::optional<Installation> discover(const DiscoveryInputs& inputs) {
    auto environment = [&](std::string_view name) -> std::string {
        if (!inputs.environment) return {};
        return without_trailing_separator(inputs.environment(name).value_or(""));
    };

    std::optional<Installation> installation;
    if (const std::string declared { environment("VCToolsInstallDir") }; !declared.empty()) installation = installation_of_tools(declared);
    if (!installation && !inputs.recordedDriver.empty()) {
        if (const std::string tools { tools_directory_of_cl(inputs.recordedDriver) }; !tools.empty()) {
            installation = installation_of_tools(tools);
        } else if (const std::string vsRoot { vs_root_of_bundled_clang(inputs.recordedDriver) }; !vsRoot.empty()) {
            installation = installation_at(vsRoot);
        }
    }
    if (!installation && inputs.run && platform::fs::is_regular_file(inputs.vswhere)) {
        const std::vector<std::string> argv { inputs.vswhere, "-products", "*", "-prerelease", "-requires",
                                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json", "-utf8" };
        if (auto output = inputs.run(argv)) {
            for (const auto& root : parse_vswhere(*output)) {
                auto candidate = installation_at(root);
                if (!candidate) continue;
                if (!installation || (candidate->hasStdModules && !installation->hasStdModules)
                    || (candidate->hasStdModules == installation->hasStdModules && newer(candidate->toolsVersion, installation->toolsVersion))) {
                    installation = std::move(candidate);
                }
            }
        }
    }
    if (!installation) return std::nullopt;

    const std::string declaredSdk { environment("WindowsSdkDir") };
    const std::string declaredVersion { environment("WindowsSDKVersion") };
    if (!declaredSdk.empty() && !declaredVersion.empty()
        && platform::fs::is_regular_file(base::join_path(declaredSdk, std::format("Include/{}/ucrt/corecrt.h", declaredVersion)))) {
        installation->sdkRoot = base::normalize_path(declaredSdk);
        installation->sdkVersion = declaredVersion;
    } else if (auto version = newest_sdk_version(declaredSdk.empty() ? inputs.defaultSdkRoot : declaredSdk)) {
        installation->sdkRoot = base::normalize_path(declaredSdk.empty() ? inputs.defaultSdkRoot : declaredSdk);
        installation->sdkVersion = *version;
    } else if (auto fallback = newest_sdk_version(inputs.defaultSdkRoot)) {
        installation->sdkRoot = base::normalize_path(inputs.defaultSdkRoot);
        installation->sdkVersion = *fallback;
    }
    return installation;
}

} // namespace mcppls::toolchain::visualstudio
