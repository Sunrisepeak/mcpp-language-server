module mcppls.devtools.version;

import std;
import mcpplibs.cmdline;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;
import mcppls.devtools.common;

namespace mcppls::devtools::version {
namespace {

namespace fs = mcppls::platform::fs;

// text[start,end) replaced by `replacement`, for writing a new value back into a file whose rest
// must survive untouched -- the C++ equivalent of Python's `text[:start] + new + text[end:]`.
std::string splice(std::string_view text, std::size_t start, std::size_t end, std::string_view replacement) {
    std::string out { text.substr(0, start) };
    out += replacement;
    out += text.substr(end);
    return out;
}

// The [start,end) span of the first quoted value on a line that literally starts with `key`,
// allowing an optional `:` or `=` and surrounding spaces/tabs between the key and the quote. This
// is the shape every quoted site here uses: mcpp.toml's `version = "..."`, package.json's (with
// `key` carrying its two-space indent) `"version": "..."`, extension.toml's `version = "..."`.
std::optional<std::pair<std::size_t, std::size_t>> quoted_line_field(std::string_view text, std::string_view key) {
    std::size_t start { 0 };
    while (start <= text.size()) {
        const auto newline = text.find('\n', start);
        const std::size_t lineEnd { newline == std::string_view::npos ? text.size() : newline };
        const std::string_view line { text.substr(start, lineEnd - start) };
        if (line.starts_with(key)) {
            std::size_t pos { key.size() };
            auto skip_space = [&] { while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos; };
            skip_space();
            if (pos < line.size() && (line[pos] == ':' || line[pos] == '=')) { ++pos; skip_space(); }
            if (pos < line.size() && line[pos] == '"') {
                const auto close = line.find('"', pos + 1);
                if (close != std::string_view::npos) return std::pair { start + pos + 1, start + close };
            }
        }
        if (newline == std::string_view::npos) break;
        start = lineEnd + 1;
    }
    return std::nullopt;
}

// The [start,end) span of the unquoted rest-of-line value after a line starting with `key`,
// trimmed -- gradle.properties' `pluginVersion = ...`.
std::optional<std::pair<std::size_t, std::size_t>> unquoted_line_field(std::string_view text, std::string_view key) {
    std::size_t start { 0 };
    while (start <= text.size()) {
        const auto newline = text.find('\n', start);
        const std::size_t lineEnd { newline == std::string_view::npos ? text.size() : newline };
        const std::string_view line { text.substr(start, lineEnd - start) };
        if (line.starts_with(key)) {
            std::size_t pos { key.size() };
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
            if (pos < line.size() && line[pos] == '=') {
                ++pos;
                while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
                std::size_t valueEnd { line.size() };
                while (valueEnd > pos && (line[valueEnd - 1] == ' ' || line[valueEnd - 1] == '\t' || line[valueEnd - 1] == '\r')) --valueEnd;
                return std::pair { start + pos, start + valueEnd };
            }
        }
        if (newline == std::string_view::npos) break;
        start = lineEnd + 1;
    }
    return std::nullopt;
}

// The [start,end) span of the quoted value inside the first `keyword { "value" }`-shaped constant
// declaration -- version.cppm's VERSION / CLANGD_VERSION / MINIMUM_MCPP_VERSION. Retried on a
// mismatch (rather than accepted on the first substring hit) because `keyword` can occur as a
// suffix of a longer name (CLANGD_VERSION also contains VERSION); this is what makes the search
// land on whichever declaration truly comes first in the file, the same as Python's unanchored
// `re.search`.
std::optional<std::pair<std::size_t, std::size_t>> braced_quoted(std::string_view text, std::string_view keyword) {
    std::size_t from { 0 };
    while (true) {
        const auto at = text.find(keyword, from);
        if (at == std::string_view::npos) return std::nullopt;
        std::size_t pos { at + keyword.size() };
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
        if (pos < text.size() && text[pos] == '{') {
            ++pos;
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
            if (pos < text.size() && text[pos] == '"') {
                const auto close = text.find('"', pos + 1);
                if (close != std::string_view::npos) return std::pair { pos + 1, close };
            }
        }
        from = at + 1;
    }
}

// Same shape, an unquoted integer instead -- kit.cppm's `KIT_VERSION { 1 }`.
std::optional<std::pair<std::size_t, std::size_t>> braced_int(std::string_view text, std::string_view keyword) {
    std::size_t from { 0 };
    while (true) {
        const auto at = text.find(keyword, from);
        if (at == std::string_view::npos) return std::nullopt;
        std::size_t pos { at + keyword.size() };
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
        if (pos < text.size() && text[pos] == '{') {
            ++pos;
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
            std::size_t digitsEnd { pos };
            while (digitsEnd < text.size() && text[digitsEnd] >= '0' && text[digitsEnd] <= '9') ++digitsEnd;
            if (digitsEnd > pos) return std::pair { pos, digitsEnd };
        }
        from = at + 1;
    }
}

base::Result<std::string> read_quoted_site(const std::string& path, std::string_view key,
                                            bool braced, const std::string& whatItIs,
                                            std::optional<std::string_view> next) {
    auto text = fs::read_file(path);
    if (!text) return std::unexpected { text.error() };
    const auto span = braced ? braced_quoted(*text, key) : quoted_line_field(*text, key);
    if (!span) return base::fail("version-site", std::format("{} has no {}", path, whatItIs));
    if (next) {
        if (auto written = fs::write_file_atomic(path, splice(*text, span->first, span->second, *next)); !written) {
            return std::unexpected { written.error() };
        }
        return std::string { *next };
    }
    return text->substr(span->first, span->second - span->first);
}

std::string manifest_path(const std::string& root) { return base::join_path(root, "mcpp.toml"); }
std::string module_path(const std::string& root) { return base::join_path(root, "modules/base/src/version.cppm"); }
std::string extension_manifest_path(const std::string& root) { return base::join_path(root, "editors/vscode/package.json"); }
std::string zed_manifest_path(const std::string& root) { return base::join_path(root, "editors/zed/extension.toml"); }
std::string clion_properties_path(const std::string& root) { return base::join_path(root, "editors/clion/gradle.properties"); }
std::string claude_plugin_path(const std::string& root) { return base::join_path(root, "editors/claude-code/mcppls-lsp/.claude-plugin/plugin.json"); }
std::string claude_marketplace_path(const std::string& root) { return base::join_path(root, "editors/claude-code/.claude-plugin/marketplace.json"); }
std::string versions_env_path(const std::string& root) { return base::join_path(root, ".github/versions.env"); }
std::string payload_lock_path(const std::string& root) { return base::join_path(root, "packaging/payload.lock.json"); }
std::string kit_module_path(const std::string& root) { return base::join_path(root, "src/spec/kit.cppm"); }
// The kit's writer: mcppls.pack.kit, which replaced packaging/scripts/build_kit.py.
std::string kit_writer_path(const std::string& root) { return base::join_path(root, "modules/pack/src/kit.cppm"); }

} // namespace

base::Result<std::string> manifest_version(const std::string& root, std::optional<std::string_view> next) {
    return read_quoted_site(manifest_path(root), "version", false, "version field", next);
}

base::Result<std::string> module_version(const std::string& root, std::optional<std::string_view> next) {
    return read_quoted_site(module_path(root), "VERSION", true, "VERSION constant", next);
}

base::Result<std::string> extension_manifest_version(const std::string& root, std::optional<std::string_view> next) {
    return read_quoted_site(extension_manifest_path(root), "  \"version\"", false, "version field", next);
}

base::Result<std::string> zed_manifest_version(const std::string& root, std::optional<std::string_view> next) {
    return read_quoted_site(zed_manifest_path(root), "version", false, "version field", next);
}

base::Result<std::string> clion_plugin_version(const std::string& root, std::optional<std::string_view> next) {
    const std::string path { clion_properties_path(root) };
    auto text = fs::read_file(path);
    if (!text) return std::unexpected { text.error() };
    const auto span = unquoted_line_field(*text, "pluginVersion");
    if (!span) return base::fail("version-site", std::format("{} has no pluginVersion", path));
    if (next) {
        if (auto written = fs::write_file_atomic(path, splice(*text, span->first, span->second, *next)); !written) {
            return std::unexpected { written.error() };
        }
        return std::string { *next };
    }
    return std::string { base::trim(text->substr(span->first, span->second - span->first)) };
}

base::Result<std::string> claude_plugin_version(const std::string& root, std::optional<std::string_view> next) {
    return read_quoted_site(claude_plugin_path(root), "  \"version\"", false, "version field", next);
}

// The plugin's entry in the marketplace: the one plugin, so the one `version` at its indent.
base::Result<std::string> claude_marketplace_version(const std::string& root, std::optional<std::string_view> next) {
    return read_quoted_site(claude_marketplace_path(root), "      \"version\"", false, "plugin version field", next);
}

base::Result<McppVersions> mcpp_versions(const std::string& root) {
    auto module = fs::read_file(module_path(root));
    if (!module) return std::unexpected { module.error() };
    auto env = fs::read_file(versions_env_path(root));
    if (!env) return std::unexpected { env.error() };

    McppVersions result {};
    if (auto span = braced_quoted(*module, "MINIMUM_MCPP_VERSION")) {
        result.minimum = module->substr(span->first, span->second - span->first);
    }
    if (auto span = unquoted_line_field(*env, "MCPP_VERSION")) {
        result.tested = std::string { base::trim(env->substr(span->first, span->second - span->first)) };
    }
    return result;
}

base::Result<ClangdVersions> clangd_versions(const std::string& root) {
    auto module = fs::read_file(module_path(root));
    if (!module) return std::unexpected { module.error() };
    auto lockText = fs::read_file(payload_lock_path(root));
    if (!lockText) return std::unexpected { lockText.error() };

    ClangdVersions result {};
    if (auto span = braced_quoted(*module, "CLANGD_VERSION")) {
        result.printed = module->substr(span->first, span->second - span->first);
    }
    try {
        const auto lock = nlohmann::json::parse(*lockText);
        if (lock.contains("clangd-version") && lock["clangd-version"].is_string()) {
            result.locked = lock["clangd-version"].get<std::string>();
        }
    } catch (const std::exception& error) {
        return base::fail("version-site", std::format("{} is not valid JSON: {}", payload_lock_path(root), error.what()));
    }
    return result;
}

base::Result<KitVersions> kit_versions(const std::string& root) {
    auto module = fs::read_file(kit_module_path(root));
    if (!module) return std::unexpected { module.error() };
    auto writer = fs::read_file(kit_writer_path(root));
    if (!writer) return std::unexpected { writer.error() };

    KitVersions result {};
    if (auto span = braced_int(*module, "KIT_VERSION")) {
        result.accepted = module->substr(span->first, span->second - span->first);
    }
    if (auto span = braced_int(*writer, "KIT_VERSION")) {
        result.written = writer->substr(span->first, span->second - span->first);
    }
    return result;
}

base::Result<std::string> extension_version(std::string_view product) {
    const auto parts = base::split(product, '.');
    // Digits only, and no leading zero but in `0` itself -- semantic versioning's own rule, which
    // vsce enforces when it packages.
    auto numeric = [](std::string_view s) {
        return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; })
            && (s.size() == 1 || s.front() != '0');
    };
    if (parts.size() == 3 && std::ranges::all_of(parts, numeric)) {
        return std::string { product };
    }
    return base::fail("version-shape",
                      std::format("'{}' is not a three-part semantic version MAJOR.MINOR.PATCH (e.g. 0.0.1)", product));
}

namespace {

std::optional<std::array<long long, 4>> version_key(std::optional<std::string> text) {
    if (!text) return std::nullopt;
    const auto parts = base::split(*text, '.');
    if (parts.size() != 4) return std::nullopt;
    std::array<long long, 4> out {};
    for (std::size_t i { 0 }; i < 4; ++i) {
        if (parts[i].empty() || !std::ranges::all_of(parts[i], [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        out[i] = std::stoll(std::string { parts[i] });
    }
    return out;
}

} // namespace

base::Result<std::vector<std::string>> check(const std::string& root) {
    auto product = manifest_version(root);
    if (!product) return std::unexpected { product.error() };
    std::vector<std::string> problems;

    auto moduleV = module_version(root);
    if (!moduleV) return std::unexpected { moduleV.error() };
    if (*moduleV != *product) {
        problems.push_back(std::format(
            "modules/base/src/version.cppm says '{}', mcpp.toml says '{}' — this is the version the running binary reports",
            *moduleV, *product));
    }

    auto wanted = extension_version(*product);
    if (!wanted) return std::unexpected { wanted.error() };

    auto extManifest = extension_manifest_version(root);
    if (!extManifest) return std::unexpected { extManifest.error() };
    if (*extManifest != *wanted) {
        problems.push_back(std::format("editors/vscode/package.json says '{}', '{}' maps to '{}'",
                                        *extManifest, *product, *wanted));
    }

    auto zedV = zed_manifest_version(root);
    if (!zedV) return std::unexpected { zedV.error() };
    if (*zedV != *wanted) {
        problems.push_back(std::format("editors/zed/extension.toml says '{}', '{}' maps to '{}'",
                                        *zedV, *product, *wanted));
    }

    auto clionV = clion_plugin_version(root);
    if (!clionV) return std::unexpected { clionV.error() };
    if (*clionV != *product) {
        problems.push_back(std::format("editors/clion/gradle.properties says '{}', mcpp.toml says '{}'", *clionV, *product));
    }

    auto claudeV = claude_plugin_version(root);
    if (!claudeV) return std::unexpected { claudeV.error() };
    if (*claudeV != *wanted) {
        problems.push_back(std::format("editors/claude-code/mcppls-lsp/.claude-plugin/plugin.json says '{}', mcpp.toml says '{}'",
                                        *claudeV, *product));
    }

    auto marketplaceV = claude_marketplace_version(root);
    if (!marketplaceV) return std::unexpected { marketplaceV.error() };
    if (*marketplaceV != *wanted) {
        problems.push_back(std::format("editors/claude-code/.claude-plugin/marketplace.json says '{}', mcpp.toml says '{}'",
                                        *marketplaceV, *product));
    }

    auto mcppV = mcpp_versions(root);
    if (!mcppV) return std::unexpected { mcppV.error() };
    if (!mcppV->minimum) {
        problems.push_back("modules/base/src/version.cppm has no MINIMUM_MCPP_VERSION");
    } else if (const auto minKey = version_key(mcppV->minimum); !minKey) {
        problems.push_back(std::format("MINIMUM_MCPP_VERSION is '{}', which is not a YYYY.M.D.N version", *mcppV->minimum));
    } else if (const auto testedKey = version_key(mcppV->tested); testedKey && *testedKey < *minKey) {
        problems.push_back(std::format(
            ".github/versions.env builds with mcpp {}, older than the MINIMUM_MCPP_VERSION {} the server tells users about",
            *mcppV->tested, *mcppV->minimum));
    }

    auto clangdV = clangd_versions(root);
    if (!clangdV) return std::unexpected { clangdV.error() };
    if (clangdV->printed != clangdV->locked) {
        problems.push_back(std::format("modules/base/src/version.cppm prints clangd '{}', packaging/payload.lock.json ships '{}'",
                                        clangdV->printed.value_or("<none>"), clangdV->locked.value_or("<none>")));
    }

    auto kitV = kit_versions(root);
    if (!kitV) return std::unexpected { kitV.error() };
    if (kitV->accepted != kitV->written) {
        problems.push_back(std::format("src/spec/kit.cppm accepts kit-version '{}', modules/pack/src/kit.cppm writes '{}'",
                                        kitV->accepted.value_or("<none>"), kitV->written.value_or("<none>")));
    }

    return problems;
}

base::Result<std::string> set_everywhere(const std::string& root, std::string_view product) {
    auto wanted = extension_version(product);
    if (!wanted) return std::unexpected { wanted.error() };
    if (auto r = manifest_version(root, product); !r) return std::unexpected { r.error() };
    if (auto r = module_version(root, product); !r) return std::unexpected { r.error() };
    if (auto r = extension_manifest_version(root, *wanted); !r) return std::unexpected { r.error() };
    if (auto r = zed_manifest_version(root, *wanted); !r) return std::unexpected { r.error() };
    if (auto r = clion_plugin_version(root, product); !r) return std::unexpected { r.error() };
    if (auto r = claude_plugin_version(root, *wanted); !r) return std::unexpected { r.error() };
    if (auto r = claude_marketplace_version(root, *wanted); !r) return std::unexpected { r.error() };
    return *wanted;
}

} // namespace mcppls::devtools::version

namespace mcppls::devtools {
namespace {

namespace cmdline = mcpplibs::cmdline;
namespace vsn = mcppls::devtools::version;

int command_version(const cmdline::ParsedArgs& arguments) {
    const std::string root { repository_root() };

    if (const auto set = arguments.value("set")) {
        auto wanted = vsn::set_everywhere(root, *set);
        if (!wanted) {
            std::println(std::cerr, "mcppls-devtools: {}", wanted.error().message);
            return 1;
        }
        std::println("version: set to {} in mcpp.toml, the server and every editor plugin", *wanted);
        return 0;
    }
    if (arguments.is_flag_set("check")) {
        auto problems = vsn::check(root);
        if (!problems) {
            std::println(std::cerr, "mcppls-devtools: {}", problems.error().message);
            return 1;
        }
        if (!problems->empty()) {
            std::println(std::cerr, "version: the version sites disagree");
            for (const auto& problem : *problems) std::println(std::cerr, "  - {}", problem);
            return 1;
        }
        auto product = vsn::manifest_version(root);
        if (!product) {
            std::println(std::cerr, "mcppls-devtools: {}", product.error().message);
            return 1;
        }
        auto extension = vsn::extension_version(*product);
        if (!extension) {
            std::println(std::cerr, "mcppls-devtools: {}", extension.error().message);
            return 1;
        }
        std::println("version: {} (extension {}), every site agrees", *product, *extension);
        return 0;
    }
    if (arguments.is_flag_set("print")) {
        auto product = vsn::manifest_version(root);
        if (!product) {
            std::println(std::cerr, "mcppls-devtools: {}", product.error().message);
            return 1;
        }
        if (arguments.is_flag_set("extension")) {
            auto extension = vsn::extension_version(*product);
            if (!extension) {
                std::println(std::cerr, "mcppls-devtools: {}", extension.error().message);
                return 1;
            }
            std::println("{}", *extension);
        } else {
            std::println("{}", *product);
        }
        return 0;
    }
    std::println(std::cerr, "mcppls-devtools: version needs one of --print, --check, --set VERSION");
    return 2;
}

} // namespace

cmdline::App version_command(bool& handled, int& status) {
    cmdline::App command { "version" };
    (void) command.description("The product version: print it, check every derived site, or set it everywhere");
    (void) command.option("print").help("Print the product version");
    (void) command.option("extension").help("With --print: the version the editor plugins carry, which is the product version");
    (void) command.option("check").help("Every derived site agrees, or say which does not");
    (void) command.option("set").takes_value().value_name("VERSION").help("Write this version everywhere");
    (void) command.action([&handled, &status](const cmdline::ParsedArgs& arguments) {
        handled = true;
        status = command_version(arguments);
    });
    return command;
}

} // namespace mcppls::devtools
