module mcppls.normalize.msvc;

import std;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.spec.database;
import mcppls.toolchain.probe;
import mcppls.normalize.gnu;

namespace mcppls::normalize {

namespace {

// Options whose value may follow as a separate argument; the build-only ones among them are dropped with it.
constexpr std::array<std::string_view, 16> SEPARATE_VALUE {
    "D", "U", "I", "FI", "external:I", "imsvc", "Tp", "Tc", "reference", "ifcOutput", "ifcSearchDir", "ifcMap",
    "headerUnit", "sourceDependencies", "sourceDependencies:directives", "scanDependencies",
};

bool is_option(std::string_view argument) { return argument.size() > 1 && (argument.front() == '/' || argument.front() == '-'); }

bool takes_separate_value(std::string_view body) {
    return std::ranges::find(SEPARATE_VALUE, body) != SEPARATE_VALUE.end() || body == "headerUnit:quote" || body == "headerUnit:angle";
}

std::string standard_of(std::string_view version) {
    if (version == "latest") return "-std=c++26";
    if (version == "23preview" || version == "23") return "-std=c++23";
    return std::format("-std=c++{}", version);
}

// A GNU-style argument clang-cl also accepts, or one passed through /clang:.
void gnu_argument(std::vector<std::string>& out, std::string_view argument) {
    static constexpr std::array<std::string_view, 6> BUILD_ONLY { "-fmodule-file=", "-fmodule-output", "-fprebuilt-module-path=",
                                                                  "-fmodules-reduced-bmi", "-fmodules", "-fexperimental-modules-reduced-bmi" };
    if (std::ranges::any_of(BUILD_ONLY, [&](std::string_view prefix) { return argument.starts_with(prefix); })) return;
    if (argument.starts_with("-f") || argument.starts_with("-m") || argument.starts_with("-Wno-") || argument.starts_with("--target=")
        || argument.starts_with("-std=") || argument.starts_with("-D") || argument.starts_with("-U") || argument.starts_with("-I")
        || argument.starts_with("-isystem") || argument.starts_with("-iquote") || argument.starts_with("-idirafter")) {
        out.emplace_back(argument);
    }
}

} // namespace

std::vector<std::string> translate_msvc(const MsvcInput& input) {
    std::vector<std::string> out { "--no-default-config" };
    const std::string source { base::normalize_path(input.source, base::PathStyle::windows) };
    std::string runtime { "static" };   // cl builds with /MT unless told otherwise
    bool standardGiven { false };
    const auto& arguments = input.arguments;

    for (std::size_t i { 1 }; i < arguments.size(); ++i) {
        const std::string_view argument { arguments[i] };
        if (argument == "/link" || argument == "-link") break;   // linker options follow
        // The unit's own source, however it is spelled; a POSIX path would otherwise read as an option.
        if (base::same_path(base::join_path(input.workDirectory, argument, base::PathStyle::windows), source, true)) continue;
        if (!is_option(argument)) continue;   // another input or an orphan value
        std::string_view body { argument.substr(1) };
        auto value = [&](std::string_view name) -> std::optional<std::string> {
            if (body.size() > name.size()) return std::string { body.substr(name.size()) };
            if (i + 1 < arguments.size()) return arguments[++i];
            return std::nullopt;
        };

        if (body.starts_with("clang:")) {
            gnu_argument(out, body.substr(6));
            continue;
        }
        // cl's own options spelled with a lowercase f: /fp: /favor: /fastfail /fsanitize=
        const bool clF { body.starts_with("fp:") || body.starts_with("favor:") || body.starts_with("fastfail") || body.starts_with("fsanitize")
                         || body.starts_with("fno-sanitize") };
        if (argument.starts_with("--target=") || argument.starts_with("-Wno-") || (argument.starts_with("-f") && !clF) || argument.starts_with("-m")) {
            gnu_argument(out, argument);
            continue;
        }
        if (takes_separate_value(body)) {
            const std::string name { body };
            const auto separate = i + 1 < arguments.size() ? std::optional<std::string> { arguments[++i] } : std::nullopt;
            if (!separate) continue;
            if (name == "D") out.push_back("-D" + *separate);
            else if (name == "U") out.push_back("-U" + *separate);
            else if (name == "I") out.push_back("-I" + *separate);
            else if (name == "FI") { out.emplace_back("-include"); out.push_back(*separate); }
            else if (name == "external:I" || name == "imsvc") { out.emplace_back("-isystem"); out.push_back(*separate); }
            continue;   // the rest are sources and build-only values
        }
        if (body.starts_with("D")) { out.push_back(std::format("-D{}", body.substr(1))); continue; }
        if (body.starts_with("U")) { out.push_back(std::format("-U{}", body.substr(1))); continue; }
        if (body.starts_with("I")) { out.push_back(std::format("-I{}", body.substr(1))); continue; }
        if (body.starts_with("FI")) {
            if (auto file = value("FI")) { out.emplace_back("-include"); out.push_back(*file); }
            continue;
        }
        if (body.starts_with("external:I")) {
            if (auto directory = value("external:I")) { out.emplace_back("-isystem"); out.push_back(*directory); }
            continue;
        }
        if (body.starts_with("imsvc")) {
            if (auto directory = value("imsvc")) { out.emplace_back("-isystem"); out.push_back(*directory); }
            continue;
        }
        if (body.starts_with("std:c++")) {
            out.push_back(standard_of(body.substr(7)));
            standardGiven = true;
            continue;
        }
        if (body.starts_with("EH")) {
            if (body.ends_with('-')) {
                out.emplace_back("-fno-cxx-exceptions");
                out.emplace_back("-fno-exceptions");
            } else {
                out.emplace_back("-fcxx-exceptions");
                out.emplace_back("-fexceptions");
            }
            continue;
        }
        if (body == "GR-") { out.emplace_back("-fno-rtti"); continue; }
        if (body == "MD") { runtime = "dll"; continue; }
        if (body == "MDd") { runtime = "dll_dbg"; continue; }
        if (body == "MT") { runtime = "static"; continue; }
        if (body == "MTd") { runtime = "static_dbg"; continue; }
        if (body == "J") { out.emplace_back("-funsigned-char"); continue; }
        if (body.starts_with("Zp")) { out.push_back(std::format("-fpack-struct={}", body.size() > 2 ? body.substr(2) : "1")); continue; }
        if (body == "Zc:alignedNew-") { out.emplace_back("-fno-aligned-allocation"); continue; }
        if (body == "Zc:char8_t-") { out.emplace_back("-fno-char8_t"); continue; }
        if (body == "Zc:char8_t") { out.emplace_back("-fchar8_t"); continue; }
        if (body == "Zc:wchar_t-") { out.emplace_back("-fno-wchar"); continue; }
        if (body == "Zc:sizedDealloc-") { out.emplace_back("-fno-sized-deallocation"); continue; }
        if (body == "Zc:twoPhase-") { out.emplace_back("-fdelayed-template-parsing"); continue; }
        if (body == "Zc:threadSafeInit-") { out.emplace_back("-fno-threadsafe-statics"); continue; }
        if (body == "Zc:trigraphs") { out.emplace_back("-ftrigraphs"); continue; }
        if (body == "arch:AVX") { out.emplace_back("-mavx"); continue; }
        if (body == "arch:AVX2") { out.emplace_back("-mavx2"); continue; }
        if (body.starts_with("arch:AVX512")) { out.emplace_back("-mavx512f"); continue; }
        // Everything else only affects code generation, diagnostics, output or module files: /c /nologo /Fo /Zi /O2 /W4
        // /interface /internalPartition /showIncludes /permissive- /utf-8 /Zc:__cplusplus ...
    }

    if (!standardGiven) out.emplace_back("-std=c++14");
    out.push_back("-fms-runtime-lib=" + runtime);
    if (input.facts != nullptr) {
        for (auto& argument : windows_msvc_arguments(*input.facts, out, input.noAlignedAllocationWithMsvcStl)) out.push_back(std::move(argument));
    }
    if (input.importable) {
        out.emplace_back("-x");
        out.emplace_back("c++-module");
    }
    return out;
}

} // namespace mcppls::normalize
