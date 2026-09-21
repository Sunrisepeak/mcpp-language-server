module mcppls.normalize.gnu;

import std;
import mcppls.base.path;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.toolchain.probe;

namespace mcppls::normalize {

namespace {

constexpr std::array<std::string_view, 6> TAKES_VALUE { "-o", "-MF", "-MT", "-MQ", "-x", "-Xclang" };
constexpr std::array<std::string_view, 7> GCC_STRIP_EXACT { "-fmodules-ts", "-fmodules", "-fmodule-only", "-fmodule-lazy",
                                                            "-fno-module-lazy", "-fno-modules", "-fno-modules-ts" };
constexpr std::array<std::string_view, 6> GCC_STRIP_PREFIX { "-fmodule-mapper=", "-fdeps-format=", "-fdeps-file=",
                                                             "-fdeps-target=", "-fmodule-header", "-flang-info-" };
constexpr std::array<std::string_view, 5> CLANG_STRIP_PREFIX { "-fmodule-file=", "-fprebuilt-module-path=", "-fmodule-output",
                                                               "-fmodules-reduced-bmi", "-fexperimental-modules-reduced-bmi" };
constexpr std::array<std::string_view, 6> ALWAYS_STRIP_EXACT { "-c", "--precompile", "-MD", "-MMD", "-MP", "-fmodules-embed-all-files" };

bool starts_with_any(std::string_view argument, std::span<const std::string_view> prefixes) {
    return std::ranges::any_of(prefixes, [&](std::string_view prefix) { return argument.starts_with(prefix); });
}

bool equals_any(std::string_view argument, std::span<const std::string_view> values) {
    return std::ranges::find(values, argument) != values.end();
}

bool has_prefix_argument(std::span<const std::string> arguments, std::string_view prefix) {
    return std::ranges::any_of(arguments, [&](const std::string& argument) { return argument.starts_with(prefix); });
}

} // namespace

std::vector<std::string> translate_gnu(const GnuInput& input) {
    const bool gcc { input.facts != nullptr && input.facts->toolchain.family == spec::Family::gcc };
    std::vector<std::string> out;
    const std::string source { base::normalize_path(input.source) };
    for (std::size_t i { 1 }; i < input.arguments.size(); ++i) {
        const std::string_view argument { input.arguments[i] };
        if (equals_any(argument, TAKES_VALUE)) {
            ++i;
            continue;
        }
        if (argument.starts_with("-o") && argument.size() > 2 && !argument.starts_with("-objc")) continue;
        if (argument.starts_with("-MF") || argument.starts_with("-MT") || argument.starts_with("-MQ")) continue;
        if (argument.starts_with("-x") && argument.size() > 2) continue;
        if (equals_any(argument, ALWAYS_STRIP_EXACT)) continue;
        // A CMake module map that was not expanded does not exist yet (it is written during the build).
        if (argument.starts_with('@') && argument.ends_with(".modmap")) continue;
        if (!argument.starts_with('-') && base::same_path(base::join_path(input.workDirectory, argument), source)) continue;
        if (starts_with_any(argument, CLANG_STRIP_PREFIX)) continue;
        if (gcc && (equals_any(argument, GCC_STRIP_EXACT) || starts_with_any(argument, GCC_STRIP_PREFIX) || argument.starts_with("-B"))) continue;
        out.emplace_back(argument);
    }
    if (gcc) {
        const auto& facts = *input.facts;
        out.emplace_back("--no-default-config");
        if (!facts.toolchain.target.empty() && !has_prefix_argument(out, "--target=")) out.push_back("--target=" + facts.toolchain.target);
        out.emplace_back("-stdlib=libstdc++");
        if (!facts.mingwRoot.empty()) {
            // Clang's MinGW driver locates GCC through the sysroot, not --gcc-install-dir (experiment E14).
            if (!has_prefix_argument(out, "--sysroot")) out.push_back("--sysroot=" + facts.mingwRoot);
        } else if (!facts.gccInstallDirectory.empty()) {
            out.push_back("--gcc-install-dir=" + facts.gccInstallDirectory);
        }
    }
    if (input.facts != nullptr && input.facts->toolchain.target.find("windows-msvc") != std::string::npos) {
        for (auto& argument : windows_msvc_arguments(*input.facts, out, input.noAlignedAllocationWithMsvcStl)) out.push_back(std::move(argument));
    }
    if (input.importable) {
        out.emplace_back("-x");
        out.emplace_back("c++-module");
    }
    return out;
}

std::vector<std::string> windows_msvc_arguments(const toolchain::ToolchainFacts& facts, std::span<const std::string> existing, bool noAlignedAllocation) {
    std::vector<std::string> out;
    auto has = [&](std::string_view prefix) {
        const auto starts = [&](const std::string& argument) { return argument.starts_with(prefix); };
        return std::ranges::any_of(existing, starts) || std::ranges::any_of(out, starts);
    };
    if (!facts.toolchain.target.empty() && !has("--target=") && !has("-target")) out.push_back("--target=" + facts.toolchain.target);
    if (!facts.msCompatibilityVersion.empty() && !has("-fms-compatibility-version=")) {
        out.push_back("-fms-compatibility-version=" + facts.msCompatibilityVersion);
    }
    if (facts.msvc) {
        if (!facts.msvc->toolsDirectory.empty() && !has("-Xmicrosoft-visualc-tools-root")) {
            out.emplace_back("-Xmicrosoft-visualc-tools-root");
            out.push_back(facts.msvc->toolsDirectory);
        }
        if (!facts.msvc->sdkRoot.empty() && !has("-Xmicrosoft-windows-sdk-root")) {
            out.emplace_back("-Xmicrosoft-windows-sdk-root");
            out.push_back(facts.msvc->sdkRoot);
            if (!facts.msvc->sdkVersion.empty()) {
                out.emplace_back("-Xmicrosoft-windows-sdk-version");
                out.push_back(facts.msvc->sdkVersion);
            }
        }
    }
    if (noAlignedAllocation && facts.toolchain.stdlib && facts.toolchain.stdlib->name == "msvc-stl" && !has("-fno-aligned-allocation") && !has("-faligned-allocation")) {
        out.emplace_back("-fno-aligned-allocation");
    }
    return out;
}

std::vector<std::string> semantic_subset(std::span<const std::string> arguments) {
    std::vector<std::string> out;
    for (std::size_t i { 1 }; i < arguments.size(); ++i) {
        const std::string_view argument { arguments[i] };
        const bool separate { argument == "-I" || argument == "-isystem" || argument == "-iquote" || argument == "-idirafter"
                              || argument == "-D" || argument == "-U" || argument == "-include" || argument == "/I" || argument == "/D"
                              || argument == "/U" || argument == "/FI" };
        if (separate && i + 1 < arguments.size()) {
            out.emplace_back(argument);
            out.push_back(arguments[++i]);
            continue;
        }
        if (argument.starts_with("-I") || argument.starts_with("-isystem") || argument.starts_with("-iquote")
            || argument.starts_with("-idirafter") || argument.starts_with("-D") || argument.starts_with("-U")
            || argument.starts_with("-std=") || argument.starts_with("-include") || argument == "-fno-exceptions"
            || argument == "-fno-rtti" || argument.starts_with("-fchar8_t") || argument.starts_with("-fno-char8_t")) {
            out.emplace_back(argument);
        } else if (argument.starts_with("/I") || argument.starts_with("/D") || argument.starts_with("/U")
                   || argument.starts_with("/FI")) {
            // MSVC spellings become GNU spellings for the Clang driver.
            out.push_back(std::format("-{}{}", argument[1] == 'F' ? "include" : std::string { argument[1] }, argument.substr(argument[1] == 'F' ? 3 : 2)));
        } else if (argument.starts_with("/std:c++")) {
            std::string_view version { argument.substr(8) };
            out.push_back(version == "latest" ? std::string { "-std=c++26" } : std::format("-std=c++{}", version));
        }
    }
    return out;
}

std::string language_standard_of(std::span<const std::string> arguments) {
    std::string standard;
    for (const auto& argument : arguments) {
        if (argument.starts_with("-std=")) standard = argument.substr(5);
        if (argument.starts_with("/std:")) standard = argument.substr(5) == "c++latest" ? "c++26" : argument.substr(5);
    }
    return standard;
}

std::vector<std::string> kit_arguments(const spec::Kit& kit, std::string_view languageStandard, std::string_view macosSdk) {
    std::vector<std::string> out { "--no-default-config", "--target=" + kit.target,
                                   std::format("-std={}", languageStandard.empty() ? "c++23" : languageStandard) };
    for (const auto& argument : kit.arguments) out.push_back(argument);
    for (const auto& directory : kit.systemIncludeDirectories) {
        out.emplace_back("-isystem");
        out.push_back(directory);
    }
    if (kit.sysroot) out.push_back("--sysroot=" + *kit.sysroot);
    if (spec::requires_macos_sdk(kit) && !macosSdk.empty()) {
        out.emplace_back("-isysroot");
        out.emplace_back(macosSdk);
    }
    return out;
}

} // namespace mcppls::normalize
