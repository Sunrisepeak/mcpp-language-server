module mcppls.project.target;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.project.compdb;
import mcppls.project.boundary;

namespace mcppls::project {

namespace {

bool target_spelling(std::string_view value) {
    // Accept future mcpp triples, but never options, whitespace or shell syntax. argv carries it literally.
    return !value.empty() && value.front() != '-' && std::ranges::all_of(value, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

bool compiler_word(std::string_view word) {
    std::string name { base::file_name(word) };
    if (name.ends_with(".exe")) name.resize(name.size() - 4);
    // A wrapper can inject options or select a different compiler. Do not interpret its argv as compiler argv.
    for (std::string_view compiler : { "clang", "clang++", "clang-cl", "gcc", "g++", "cc", "c++", "cl" }) {
        if (name == compiler) return true;
        const std::string versioned { std::string { compiler } + "-" };
        if (name.starts_with(versioned) && name.size() > versioned.size()
            && std::ranges::all_of(std::string_view { name }.substr(versioned.size()), [](char c) {
                return (c >= '0' && c <= '9') || c == '.';
            })) return true;
    }
    return false;
}

} // namespace

namespace {

std::string canonical_target(std::string_view value) {
    // These omissions only change the vendor spelling, never architecture, OS, ABI or version.
    for (std::string_view arch : { "i686", "x86_64", "aarch64" }) {
        if (value == std::string { arch } + "-windows-msvc" || value == std::string { arch } + "-unknown-windows-msvc")
            return std::string { arch } + "-pc-windows-msvc";
        if (value == std::string { arch } + "-windows-gnu" || value == std::string { arch } + "-unknown-windows-gnu")
            return std::string { arch } + "-w64-windows-gnu";
        for (std::string_view abi : { "gnu", "musl" }) {
            if (value == std::format("{}-linux-{}", arch, abi)) return std::format("{}-unknown-linux-{}", arch, abi);
        }
    }
    return std::string { value };
}

} // namespace

bool equivalent_mcpp_targets(std::string_view left, std::string_view right) {
    return canonical_target(left) == canonical_target(right);
}

bool mcpp_target_matches(std::string_view requested, std::string_view effective) {
    if (equivalent_mcpp_targets(requested, effective)) return true;
    // mcpp's abstract macOS selector deliberately leaves the deployment version to the manifest.
    // This is request compatibility, not triple equivalence: concrete versions remain distinct keys.
    for (std::string_view arch : { "aarch64", "x86_64" }) {
        if (requested != std::string { arch } + "-macos") continue;
        const std::string compilerArch { arch == "aarch64" ? "arm64" : "x86_64" };
        for (const auto& prefix : {compilerArch + "-apple-macos", compilerArch + "-apple-darwin", std::string { arch } + "-apple-darwin"}) {
            if (!effective.starts_with(prefix)) continue;
            const auto version = effective.substr(prefix.size());
            if (std::ranges::all_of(version, [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) return true;
        }
    }
    return false;
}

std::string target_selection_key(const TargetSelection& selection) {
    return selection.mode + "\n" + canonical_target(selection.requestedTarget);
}

bool valid_mcpp_target_selector(std::string_view value) { return target_spelling(value); }

TargetSelection target_from_commands(std::string_view root, std::span<const CompileCommand> commands) {
    TargetSelection selected;
    selected.evidencePath = base::join_path(root, "compile_commands.json");
    const ProjectBoundaries boundaries { root };
    std::size_t included { 0 };
    for (const auto& command : commands) {
        if (!boundaries.owns(command.file)) continue;
        // build.mcpp is mcpp's host build program, including a workspace member's build program.
        if (base::file_name(command.file) == "build.mcpp") continue;
        ++included;
        const auto& args = command.arguments;
        auto unknown = [&](std::string why) {
            selected.requestedTarget.clear();
            selected.reason = std::move(why);
            return selected;
        };
        if (args.empty() || !compiler_word(args.front())) return unknown("a project command uses an unrecognized compiler or wrapper");
        std::string target;
        for (std::size_t i { 1 }; i < args.size(); ++i) {
            const std::string& arg { args[i] };
            if (arg.starts_with('@') || arg == "-m16" || arg == "-m32" || arg == "-m64" || arg == "-mx32" || arg.starts_with("-mabi")
                || arg == "-mbig-endian" || arg == "-mlittle-endian" || arg == "-EB" || arg == "-EL" || arg.starts_with("-arch")
                || arg == "--" || arg.starts_with("--config") || arg.starts_with("-target=") || arg.starts_with("-Xarch")
                || arg.starts_with("-ccc-host-triple") || arg == "-Xclang" || arg.starts_with("/clang:") || arg == "&&" || arg == ";" || arg == "||" || arg == "|") {
                return unknown("a project command contains an unresolved target-affecting argument");
            }
            std::string value;
            if (arg.starts_with("--target=")) value = arg.substr(9);
            else if (arg == "--target" || arg == "-target") {
                if (++i == args.size()) return unknown("a target option is missing its value");
                value = args[i];
            } else continue;
            if (!target_spelling(value)) return unknown("a project command has an invalid target");
            if (!target.empty() && !equivalent_mcpp_targets(target, value)) return unknown("a project command has conflicting target options");
            target = std::move(value);
        }
        if (target.empty()) return unknown("a project command has no explicit target");
        if (!selected.requestedTarget.empty() && !equivalent_mcpp_targets(selected.requestedTarget, target)) return unknown("project commands contain multiple targets");
        selected.requestedTarget = std::move(target);
    }
    if (included == 0) selected.reason = "the database contains no identifiable project translation units";
    else {
        selected.source = "compile-commands";
        selected.reason = "all project commands name the same explicit target";
    }
    return selected;
}

TargetSelection select_mcpp_target(std::string_view root, std::string_view selector) {
    TargetSelection selected;
    selected.evidencePath = base::join_path(root, "compile_commands.json");
    if (selector == "project") {
        selected.evidencePath.clear();
        selected.mode = "project";
        selected.reason = "the workspace selects mcpp project defaults";
        return selected;
    }
    if (selector != "auto") {
        selected.evidencePath.clear();
        selected.mode = "explicit";
        selected.source = "configuration";
        if (valid_mcpp_target_selector(selector)) {
            selected.requestedTarget = selector;
            selected.reason = "the workspace fixes the analysis target";
        } else selected.reason = "invalid mcpp target setting";
        return selected;
    }
    if (!platform::fs::exists(selected.evidencePath)) {
        selected.reason = "no root compilation database is available";
        return selected;
    }
    // This path also runs during file events. Bound memory/work before parsing and never run a command.
    const auto stamp = platform::fs::stamp(selected.evidencePath);
    if (!stamp || stamp->size > 32 * 1024 * 1024) {
        selected.reason = "the root compilation database exceeds the automatic selection size limit";
        return selected;
    }
    const auto text = platform::fs::read_file(selected.evidencePath, 32 * 1024 * 1024);
    if (!text || text->size() > 32 * 1024 * 1024) {
        selected.retryable = true;
        selected.reason = "the root compilation database cannot be read within the automatic selection limit";
        return selected;
    }
    if (platform::fs::stamp(selected.evidencePath) != stamp) {
        selected.retryable = true;
        selected.reason = "the root compilation database changed while being read";
        return selected;
    }
    const auto document = nlohmann::json::parse(*text, nullptr, false);
    if (!document.is_array() || document.size() > 100000) {
        selected.retryable = document.is_discarded();
        selected.reason = "the root compilation database is invalid or exceeds the entry limit";
        return selected;
    }
    // The general importer tolerates bad rows. Target uniqueness must never silently skip one.
    for (const auto& entry : document) {
        if (!entry.is_object() || !entry.contains("directory") || !entry["directory"].is_string()
            || !entry.contains("file") || !entry["file"].is_string()
            || entry["directory"].get_ref<const std::string&>().empty() || entry["file"].get_ref<const std::string&>().empty()
            || (entry.contains("output") && !entry["output"].is_string())) {
            selected.reason = "the root compilation database contains an invalid entry";
            return selected;
        }
        if (entry.contains("arguments")) {
            if (!entry["arguments"].is_array() || entry["arguments"].empty()
                || !std::ranges::all_of(entry["arguments"], [](const auto& arg) { return arg.is_string(); })) {
                selected.reason = "the root compilation database contains invalid arguments";
                return selected;
            }
        } else if (!entry.contains("command") || !entry["command"].is_string() || entry["command"].get_ref<const std::string&>().empty()) {
            selected.reason = "the root compilation database contains no usable command";
            return selected;
        }
    }
    // Reuse the platform-aware splitter (including Windows quoting) without expanding response files.
    auto commands = parse_compile_commands(document, mcppls::os::family() == mcppls::os::Family::windows ? CommandSyntax::windows : CommandSyntax::posix);
    if (!commands) {
        selected.reason = "the root compilation database cannot be read: " + commands.error().message;
        return selected;
    }
    return target_from_commands(root, *commands);
}

} // namespace mcppls::project
