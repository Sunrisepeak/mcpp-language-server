module mcppls.pack.openkal;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcpplibs.os;

namespace os = mcpplibs::os;

namespace mcppls::pack::openkal {
namespace {

// A unit's arguments: `arguments` as the database gives them, or `command` split on blanks.
std::vector<std::string> arguments_of(const nlohmann::json& entry) {
    std::vector<std::string> arguments;
    if (const auto given = entry.find("arguments"); given != entry.end() && given->is_array()) {
        for (const auto& argument : *given) {
            if (argument.is_string()) arguments.push_back(argument.get<std::string>());
        }
        return arguments;
    }
    for (const auto word : base::split(entry.value("command", std::string {}), ' ')) {
        if (!word.empty()) arguments.emplace_back(word);
    }
    return arguments;
}

std::string file_of(const nlohmann::json& entry) {
    std::string file { entry.value("file", std::string {}) };
    if (!file.empty() && !base::is_absolute_path(file)) file = base::join_path(entry.value("directory", std::string {}), file);
    return base::normalize_path(file);
}

// The directories one flag of a command names, in order, joined (`-I<dir>`) or apart (`-I <dir>`).
std::vector<std::string> flag_directories(std::span<const std::string> arguments, std::string_view flag) {
    std::vector<std::string> directories;
    for (std::size_t i { 0 }; i < arguments.size(); ++i) {
        const std::string& argument { arguments[i] };
        if (!argument.starts_with(flag)) continue;
        if (argument.size() > flag.size()) directories.push_back(base::normalize_path(argument.substr(flag.size())));
        else if (i + 1 < arguments.size()) directories.push_back(base::normalize_path(arguments[++i]));
    }
    return directories;
}

// A run-time's own sources: a unit includes them only to build the run-time.
bool private_directory(std::string_view directory) {
    const std::string name { base::file_name(directory) };
    if (name == "src") return true;
    return name == "libc" && base::file_name(base::parent_path(directory)) == "llvm";
}

bool version_like(std::string_view text) {
    return !text.empty() && std::isdigit(static_cast<unsigned char>(text.front())) != 0
           && std::ranges::all_of(text, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.'; });
}

// Where a directory of a package goes in the kit: `<package>/<path inside it>`, the package named
// without its version (`.../mcpplibs-x-openkal-musl/0.22.6/openkal-musl-0.22.6/musl/include` is
// `openkal-musl/musl/include`). A directory of no package keeps its own name.
std::string kit_place(std::string_view directory) {
    std::vector<std::string> parts;
    for (const auto part : base::split(base::normalize_path(directory), '/')) {
        if (!part.empty()) parts.emplace_back(part);
    }
    for (std::size_t i { 0 }; i + 1 < parts.size(); ++i) {
        if (!version_like(parts[i]) || !parts[i + 1].ends_with("-" + parts[i])) continue;
        std::string place { parts[i + 1].substr(0, parts[i + 1].size() - parts[i].size() - 1) };
        for (std::size_t j { i + 2 }; j < parts.size(); ++j) place += "/" + parts[j];
        return place;
    }
    return parts.empty() ? std::string { "include" } : parts.back();
}

// The package root a directory lies in (see kit_place), or empty.
std::string package_root(std::string_view directory) {
    std::string root;
    std::string walked;
    for (const auto part : base::split(base::normalize_path(directory), '/')) {
        const std::string previous { base::file_name(walked) };
        walked = walked.empty() && part.empty() ? std::string { "/" } : base::join_path(walked, part);
        if (!part.empty() && version_like(previous) && std::string_view { part }.ends_with("-" + previous)) root = walked;
    }
    return root;
}

// What decides how a unit reads, of its arguments: defines, the width of wchar_t and which standard
// headers are not the toolchain's.
std::vector<std::string> reading_arguments(std::span<const std::string> arguments) {
    std::vector<std::string> kept;
    for (std::size_t i { 0 }; i < arguments.size(); ++i) {
        const std::string& argument { arguments[i] };
        if (argument == "-D" || argument == "-U") {
            if (i + 1 < arguments.size()) kept.push_back(argument + arguments[++i]);
            continue;
        }
        if (argument.starts_with("-D") || argument.starts_with("-U") || argument == "-fno-short-wchar" || argument == "-fshort-wchar"
            || argument == "-nostdinc++" || argument == "-nostdlibinc") {
            kept.push_back(argument);
        }
    }
    return kept;
}

std::string triple_of(std::span<const std::string> arguments) {
    for (std::size_t i { 0 }; i < arguments.size(); ++i) {
        if (arguments[i].starts_with("--target=")) return arguments[i].substr(9);
        if (arguments[i] == "-target" && i + 1 < arguments.size()) return arguments[i + 1];
    }
    return {};
}

base::Result<void> copy_directory(const std::string& source, const std::string& destination) {
    std::error_code failed;
    if (!std::filesystem::create_directories(destination, failed) && failed) {
        return base::fail("create-directory", std::format("cannot create {}: {}", destination, failed.message()));
    }
    std::filesystem::copy(std::filesystem::path { source }, std::filesystem::path { destination },
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, failed);
    if (failed) return base::fail("kit-copy", std::format("cannot copy {} to {}: {}", source, destination, failed.message()));
    return {};
}

base::Result<void> copy_file(const std::string& source, const std::string& destination) {
    std::error_code failed;
    if (!std::filesystem::create_directories(base::parent_path(destination), failed) && failed) {
        return base::fail("create-directory", std::format("cannot create {}: {}", base::parent_path(destination), failed.message()));
    }
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, failed);
    if (failed) return base::fail("kit-copy", std::format("cannot copy {} to {}: {}", source, destination, failed.message()));
    return {};
}

bool license_name(std::string_view name) {
    const std::string upper { [&] {
        std::string out { name };
        for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }() };
    return upper.starts_with("LICENSE") || upper.starts_with("COPYRIGHT") || upper.starts_with("COPYING") || upper.starts_with("NOTICE");
}

// The license texts directly in `directory`, kept as `licenses/<package>/<path in the package>`.
void licenses_in(const std::string& directory, const std::string& root, std::string_view package, std::set<std::string>& seen,
                 std::vector<std::pair<std::string, std::string>>& found) {
    std::error_code ignored;
    for (const auto& child : base::children(directory)) {
        if (!license_name(base::file_name(child)) || !std::filesystem::is_regular_file(child, ignored) || !seen.insert(child).second) continue;
        const auto relative = base::relative_path(child, root);
        found.emplace_back(child, std::format("licenses/{}/{}", package, relative.value_or(std::string { base::file_name(child) })));
    }
}

// The license texts of the packages the kit copies from: the package root's, and those in the
// directories between it and each directory copied (libc++'s LICENSE.TXT, musl's COPYRIGHT).
std::vector<std::pair<std::string, std::string>> license_files(std::span<const Copy> copies) {
    std::set<std::string> seen;
    std::vector<std::pair<std::string, std::string>> found;   // source, kit path
    for (const auto& copy : copies) {
        const std::string root { package_root(copy.source) };
        if (root.empty()) continue;
        const std::string place { kit_place(root + "/x") };
        const std::string package { place.substr(0, place.find('/')) };
        // From the directory above the one copied up to the package's root.
        std::string directory { base::parent_path(copy.source) };
        while (directory.size() >= root.size()) {
            licenses_in(directory, root, package, seen, found);
            if (directory == root || base::parent_path(directory) == directory) break;
            directory = base::parent_path(directory);
        }
    }
    return found;
}

std::uint64_t size_of_tree(const std::string& root, std::uint64_t& files) {
    std::uint64_t bytes { 0 };
    std::error_code failed;
    for (auto it = std::filesystem::recursive_directory_iterator(root, failed); !failed && it != std::filesystem::recursive_directory_iterator();
         it.increment(failed)) {
        if (!it->is_regular_file(failed)) continue;
        ++files;
        bytes += it->file_size(failed);
    }
    return bytes;
}

} // namespace

base::Result<TargetCommand> command_of(std::string_view name, const nlohmann::json& database, std::string_view projectRoot) {
    if (!database.is_array()) return base::fail("openkal-kit", std::format("the build database of {} is not a list of compile commands", name));
    TargetCommand command { .name = std::string { name } };
    const std::string root { base::normalize_path(projectRoot) };
    for (const auto& entry : database) {
        if (!entry.is_object()) continue;
        const std::string file { file_of(entry) };
        const std::string fileName { base::file_name(file) };
        if (fileName == "std.cppm" && command.stdModule.empty()) {
            command.stdModule = file;
            const auto arguments = arguments_of(entry);
            command.includes = flag_directories(arguments, "-isystem");
            command.stdIncludes = flag_directories(arguments, "-I");
        } else if (fileName == "std.compat.cppm" && command.stdCompatModule.empty()) {
            command.stdCompatModule = file;
        } else if (command.arguments.empty() && base::is_within(file, root)) {
            auto arguments = arguments_of(entry);
            if (!arguments.empty()) arguments.erase(arguments.begin());   // the compiler
            std::erase(arguments, entry.value("file", std::string {}));
            std::erase(arguments, file);
            command.arguments = std::move(arguments);
        }
    }
    if (command.arguments.empty()) return base::fail("openkal-kit", std::format("the build database of {} has no unit under {}", name, root));
    if (command.stdModule.empty()) return base::fail("openkal-kit", std::format("the build database of {} names no std module source", name));
    // A build that imports no std.compat does not describe it; the run-time keeps it beside std.cppm.
    if (command.stdCompatModule.empty()) {
        std::error_code ignored;
        const std::string beside { base::join_path(base::parent_path(command.stdModule), "std.compat.cppm") };
        if (std::filesystem::is_regular_file(beside, ignored)) command.stdCompatModule = beside;
    }
    return command;
}

base::Result<Layout> plan(std::span<const TargetCommand> commands) {
    if (commands.empty()) return base::fail("openkal-kit", "no target to make the kit for");
    Layout layout;
    std::map<std::string, int> readers;   // directory -> how many targets read it
    std::vector<std::vector<std::string>> directories;
    for (const auto& command : commands) {
        std::vector<std::string> kept;
        for (const auto& directory : command.includes) {
            if (private_directory(directory) || std::ranges::find(kept, directory) != kept.end()) continue;
            kept.push_back(directory);
            ++readers[directory];
        }
        directories.push_back(std::move(kept));
    }
    std::map<std::string, std::string> placed;   // source -> destination
    for (std::size_t t { 0 }; t < commands.size(); ++t) {
        const auto& command = commands[t];
        TargetLayout target { .name = command.name, .triple = triple_of(command.arguments), .arguments = reading_arguments(command.arguments) };
        if (target.triple.empty()) return base::fail("openkal-kit", std::format("the unit of {} names no --target", command.name));
        for (const auto& directory : directories[t]) {
            const bool shared { readers[directory] == static_cast<int>(commands.size()) };
            const std::string destination { shared ? std::format("include/{}", kit_place(directory))
                                                   : std::format("include/{}/{}", command.name, kit_place(directory)) };
            if (const auto [at, inserted] = placed.emplace(directory, destination); inserted) {
                layout.copies.push_back(Copy { directory, destination });
            }
            target.includes.push_back(destination);
        }
        layout.targets.push_back(std::move(target));
    }
    return layout;
}

std::optional<std::string> libcxx_version(std::span<const std::string> includes) {
    for (const auto& directory : includes) {
        const auto text = os::fs::read(base::join_path(directory, "__config"));
        if (!text) continue;
        for (const auto line : base::split_lines(*text)) {
            const std::string_view trimmed { base::trim(line) };
            if (!trimmed.starts_with("#")) continue;
            const auto at = trimmed.find("define _LIBCPP_VERSION ");
            if (at == std::string_view::npos) continue;
            const std::string_view number { base::trim(trimmed.substr(at + 23)) };
            int value { 0 };
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
            if (parsed.ec != std::errc {} || value < 10000) continue;
            // XXYYZZ: LLVM XX.YY.ZZ.
            return std::format("{}.{}.{}", value / 10000, (value / 100) % 100, value % 100);
        }
    }
    return std::nullopt;
}

base::Result<BuiltKit> build(const Options& options) {
    std::error_code ignored;
    auto layout = plan(options.commands);
    if (!layout) return std::unexpected { layout.error() };
    const std::string out { base::normalize_path(options.outDir) };
    std::filesystem::remove_all(out, ignored);
    if (std::error_code failed; !std::filesystem::create_directories(out, failed) && failed) {
        return base::fail("create-directory", std::format("cannot create {}: {}", out, failed.message()));
    }
    for (const auto& copy : layout->copies) {
        if (!std::filesystem::is_directory(copy.source, ignored)) {
            return base::fail("openkal-kit", std::format("{} is not there: build the targets first (mcpp build --target openkal)", copy.source));
        }
        if (auto copied = copy_directory(copy.source, base::join_path(out, copy.destination)); !copied) return std::unexpected { copied.error() };
    }

    // The std modules: their sources beside the `std/*.inc` files they include, and a manifest for them.
    const auto& first = options.commands.front();
    for (const auto& command : options.commands) {
        if (command.stdModule != first.stdModule) {
            return base::fail("openkal-kit", std::format("the targets compile different std modules ({} and {})", first.stdModule, command.stdModule));
        }
    }
    const std::string modules { base::join_path(out, "share/libc++/v1") };
    if (auto copied = copy_file(first.stdModule, base::join_path(modules, "std.cppm")); !copied) return std::unexpected { copied.error() };
    nlohmann::ordered_json manifestModules = nlohmann::ordered_json::array();
    manifestModules.push_back({ { "logical-name", "std" }, { "source-path", "../share/libc++/v1/std.cppm" }, { "is-std-library", true } });
    if (!first.stdCompatModule.empty()) {
        if (auto copied = copy_file(first.stdCompatModule, base::join_path(modules, "std.compat.cppm")); !copied) return std::unexpected { copied.error() };
        manifestModules.push_back({ { "logical-name", "std.compat" }, { "source-path", "../share/libc++/v1/std.compat.cppm" }, { "is-std-library", true } });
    }
    bool parts { false };
    for (const auto& directory : first.stdIncludes) {
        if (!std::filesystem::is_directory(base::join_path(directory, "std"), ignored)) continue;
        for (std::string_view part : { "std", "std.compat" }) {
            const std::string source { base::join_path(directory, part) };
            if (!std::filesystem::is_directory(source, ignored)) continue;
            if (auto copied = copy_directory(source, base::join_path(modules, part)); !copied) return std::unexpected { copied.error() };
        }
        parts = true;
        break;
    }
    if (!parts) return base::fail("openkal-kit", std::format("no directory of the std module's command holds its std/*.inc ({})", first.stdModule));
    (void)std::filesystem::create_directories(base::join_path(out, "lib"), ignored);
    const nlohmann::ordered_json moduleManifest { { "version", 1 }, { "revision", 1 }, { "modules", manifestModules } };
    if (auto written = os::fs::write(base::join_path(out, "lib/libc++.modules.json"), moduleManifest.dump(2) + "\n"); !written) {
        return base::fail("write-file", written.error());
    }

    std::vector<std::string> licenses;
    for (const auto& [source, kitPath] : license_files(layout->copies)) {
        if (auto copied = copy_file(source, base::join_path(out, kitPath)); !copied) return std::unexpected { copied.error() };
        licenses.push_back(kitPath);
    }
    if (licenses.empty()) return base::fail("openkal-kit", "the packages the kit copies from carry no license text");

    // S4-4-5: the kit's libc++ is the version its headers are.
    std::vector<std::string> copiedIncludes;
    for (const auto& include : layout->targets.front().includes) copiedIncludes.push_back(base::join_path(out, include));
    const auto version = libcxx_version(copiedIncludes);
    if (!version) return base::fail("openkal-kit", "no __config of the kit's headers states _LIBCPP_VERSION");

    const auto& primary = layout->targets.front();
    const std::string name { std::format("mcppls-kit-openkal-{}", options.version) };
    nlohmann::ordered_json kit;
    kit["kit-version"] = 2;
    kit["name"] = name;
    kit["target"] = primary.triple;
    kit["stdlib"] = { { "name", "libc++" }, { "version", *version }, { "module-metadata", "lib/libc++.modules.json" } };
    kit["system-include-directories"] = primary.includes;
    kit["sysroot"] = nullptr;
    kit["arguments"] = primary.arguments;
    nlohmann::ordered_json targets = nlohmann::ordered_json::object();
    for (const auto& target : layout->targets) {
        targets[target.name] = { { "target", target.triple }, { "system-include-directories", target.includes }, { "arguments", target.arguments } };
    }
    kit["targets"] = targets;
    kit["licenses"] = licenses;
    if (auto written = os::fs::write(base::join_path(out, "kit.json"), kit.dump(2) + "\n"); !written) return base::fail("write-file", written.error());

    BuiltKit built { .kitDir = out, .name = name, .libcxxVersion = *version };
    built.totalBytes = size_of_tree(out, built.fileCount);
    return built;
}

} // namespace mcppls::pack::openkal
