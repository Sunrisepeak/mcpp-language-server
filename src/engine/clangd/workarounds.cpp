module mcppls.engine.clangd.workarounds;

import std;
import mcppls.base.text;

namespace mcppls::engine::clangd {

namespace {

constexpr std::array<Workaround, 7> REGISTRY { {
    {
        .id = TRAILING_DOT_MODULE_NAME,
        .title = "a module name ending in '.' at the end of its line spins clangd forever; clangd is given the line with ';' after the dot",
        .fixedIn = "",
        .upstream = "unfiled; fixed on llvm-project main, likely by 6dcfc17b1b (#187846), not in 23.1.2",
        .evidence = ".agents/docs/2026-09-25-import-hang-status-highlight.md §1; conformance fixtures typing-import, workaround-canaries",
        .added = "0.0.4",
        .removeWhen = "the bundled clangd and the oldest clangd the server supports finish `import a.` at once",
        .canary = "conformance/fixtures/workaround-canaries: clangd --check on `import hello.` does not finish",
        .premise = "clangd reads a file only as it is given; it also reads the file's imports from disk (UP-14), so the server checks what is on disk on every save and watched change (fix plan F16)",
    },
    {
        .id = UNRESOLVED_IMPORT_STAND_INS,
        .title = "building a module unit whose import resolves to nothing can deadlock clangd; such imports get an empty stand-in unit",
        .fixedIn = "",
        .upstream = "unfiled",
        .evidence = "robustness design C2, experiments S12 and S17; conformance fixture module-faults",
        .added = "0.0.1",
        .removeWhen = "clangd builds a unit with an unresolved import to a diagnostic instead of stalling",
        .canary = "",
        .premise = "every module a unit imports has a unit in the engine database, real or a stand-in",
    },
    {
        .id = MODULE_PREPARATION,
        .title = "clangd builds the modules a file needs one file at a time; the server prepares them in parallel first",
        .fixedIn = "",
        .upstream = "unfiled",
        .evidence = "cold-start plan 4.4; conformance fixture timing",
        .added = "0.0.1",
        .removeWhen = "clangd builds independent modules concurrently on its own",
        .canary = "",
        .premise = "clangd builds a module once and reuses its BMI for every file that imports it",
    },
    {
        .id = MODULE_HINTS,
        .title = "clangd scans every file of the database to find a module's unit; the database names each unit instead",
        .fixedIn = "",
        .upstream = "unfiled (ProjectModules.cpp, CompileCommandsProjectModules)",
        .evidence = "usable plan W7; conformance fixture timing",
        .added = "0.0.1",
        .removeWhen = "clangd looks a module's unit up without scanning the whole database",
        .canary = "",
        .premise = "clangd finds a module's unit through the -fmodule-file hint in the importing unit's command",
    },
    {
        .id = MSVC_STL_ALIGNED_ALLOCATION,
        .title = "clangd rejects the MSVC STL's aligned allocation; units using the MSVC STL turn it off",
        .fixedIn = "23.1.1",
        .upstream = "llvm-project#218152, fixed in 23.1.1",
        .evidence = ".agents/docs/design.md §7; conformance fixtures cmake-msvc-std, mcpp-msvc",
        .added = "0.0.1",
        .removeWhen = "the bundled clangd is 23.1.1 or later",
        .canary = "",
        .premise = "the unit is compiled against the MSVC STL",
    },
    {
        .id = DIRECTIVE_SEMICOLON_POSITION,
        .title = "an import or module directive missing its ';' is reported on the next line of code; the diagnostic is moved back to the directive",
        .fixedIn = "",
        .upstream = "unfiled (UP-15 in issue #24)",
        .evidence = ".agents/docs/2026-09-26-issue-23-fix-plan.md F12; tests/test_workarounds.cpp",
        .added = "0.0.5",
        .removeWhen = "clangd reports expected_semi_after_module_or_import and pp_unexpected_tok_after_module_name on the directive's own line",
        .canary = "",
        .premise = "the nearest non-blank line above the diagnostic is the directive that lacks the ';'",
    },
    {
        .id = UNSAVED_IMPORT_NOT_FOUND,
        .title = "clangd reads an open file's imports from disk, so an import only in the unsaved buffer is 'not found'; told as information while the module is in the project",
        .fixedIn = "",
        .upstream = "unfiled (UP-14 in issue #24)",
        .evidence = ".agents/docs/2026-09-26-issue-23-fix-plan.md F11; tests/test_workarounds.cpp",
        .added = "0.0.5",
        .removeWhen = "clangd scans an open file's imports from its buffer (ModuleDependencyScanner through the dirty-buffer file system)",
        .canary = "",
        .premise = "the module is provided by a unit of the engine database, and the import is in the buffer but not on disk",
    },
} };

// "23.1.0" -> {23, 1, 0}; anything else -> nullopt.
std::optional<std::array<int, 3>> parse_version(std::string_view version) {
    std::array<int, 3> parts {};
    for (std::size_t i { 0 }; i < parts.size(); ++i) {
        const auto end { version.find('.') };
        const std::string_view part { version.substr(0, end) };
        const auto [ptr, error] { std::from_chars(part.data(), part.data() + part.size(), parts[i]) };
        if (error != std::errc {} || ptr != part.data() + part.size() || part.empty()) return std::nullopt;
        if (i + 1 < parts.size()) {
            if (end == std::string_view::npos) return std::nullopt;
            version.remove_prefix(end + 1);
        } else if (end != std::string_view::npos) {
            return std::nullopt;
        }
    }
    return parts;
}

bool in_known_line(std::string_view version) {
    return version.starts_with(KNOWN_LINE) && version.size() > KNOWN_LINE.size() && version[KNOWN_LINE.size()] == '.';
}

// A byte of a UTF-8 sequence is part of a name: C++ identifiers may be written in any script.
bool name_char(char c) { return base::is_identifier_char(c) || c == '.' || c == ':' || static_cast<unsigned char>(c) >= 0x80; }

bool keyword_at(std::string_view line, std::size_t at, std::string_view keyword) {
    if (line.substr(at, keyword.size()) != keyword) return false;
    const std::size_t after { at + keyword.size() };
    return after == line.size() || !base::is_identifier_char(line[after]);
}

std::size_t skip_blanks(std::string_view line, std::size_t at) {
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
    return at;
}

// Where `;` goes on `line` (after its dot), or npos. `line` has no line terminator.
std::size_t insertion_point(std::string_view line) {
    std::size_t at { skip_blanks(line, 0) };
    if (keyword_at(line, at, "export")) at = skip_blanks(line, at + 6);
    if (keyword_at(line, at, "import")) {
        at += 6;
    } else if (keyword_at(line, at, "module")) {
        at += 6;
    } else {
        return std::string_view::npos;
    }
    const std::size_t nameStart { skip_blanks(line, at) };
    if (nameStart == at || nameStart >= line.size()) return std::string_view::npos;
    std::size_t end { nameStart };
    while (end < line.size() && name_char(line[end])) ++end;
    if (end == nameStart || line[end - 1] != '.') return std::string_view::npos;
    // Nothing but blanks or a comment may follow the dot on this line.
    const std::size_t rest { skip_blanks(line, end) };
    if (rest < line.size() && !line.substr(rest).starts_with("//") && !line.substr(rest).starts_with("/*")) return std::string_view::npos;
    return end;
}

} // namespace

std::span<const Workaround> workarounds() { return REGISTRY; }

const Workaround* find_workaround(std::string_view id) {
    const auto found { std::ranges::find(REGISTRY, id, &Workaround::id) };
    return found == REGISTRY.end() ? nullptr : &*found;
}

bool needs(const Workaround& workaround, std::string_view version) {
    if (!in_known_line(version)) return true;
    if (workaround.fixedIn.empty()) return true;
    const auto have { parse_version(version) };
    const auto fixed { parse_version(workaround.fixedIn) };
    if (!have || !fixed) return true;
    return *have < *fixed;
}

bool needs(std::string_view id, std::string_view version) {
    const Workaround* workaround { find_workaround(id) };
    return workaround != nullptr && needs(*workaround, version);
}

std::vector<std::string_view> active_workarounds(std::string_view version) {
    std::vector<std::string_view> ids;
    for (const auto& workaround : REGISTRY) {
        if (needs(workaround, version)) ids.push_back(workaround.id);
    }
    return ids;
}

Sanitized sanitize_module_names(std::string_view text) {
    Sanitized result;
    bool inBlockComment { false };
    int lineNumber { 0 };
    std::size_t lineStart { 0 };
    std::size_t copiedUpTo { 0 };
    while (lineStart <= text.size()) {
        std::size_t lineEnd { text.find('\n', lineStart) };
        const bool last { lineEnd == std::string_view::npos };
        if (last) lineEnd = text.size();
        std::string_view line { text.substr(lineStart, lineEnd - lineStart) };
        if (line.ends_with('\r')) line.remove_suffix(1);
        // A directive inside a block comment is no directive; one that opens a comment afterwards still is.
        const bool startsInComment { inBlockComment };
        for (std::size_t at { 0 }; at + 1 < line.size(); ++at) {
            if (!inBlockComment && line[at] == '/' && line[at + 1] == '/') break;
            if (!inBlockComment && line[at] == '/' && line[at + 1] == '*') {
                inBlockComment = true;
                ++at;
            } else if (inBlockComment && line[at] == '*' && line[at + 1] == '/') {
                inBlockComment = false;
                ++at;
            }
        }
        if (!startsInComment) {
            // A file read from disk can begin with a UTF-8 byte order mark (fix plan F2): it is no part of the directive.
            const std::size_t mark { lineNumber == 0 ? base::byte_order_mark_size(line) : 0 };
            if (std::size_t point { insertion_point(line.substr(mark)) }; point != std::string_view::npos) {
                point += mark;
                const std::size_t offset { lineStart + point };
                result.text.append(text.substr(copiedUpTo, offset - copiedUpTo));
                result.text.push_back(';');
                copiedUpTo = offset;
                result.insertions.push_back(Insertion { lineNumber, static_cast<int>(base::utf16_length(line.substr(0, point))) });
            }
        }
        if (last) break;
        lineStart = lineEnd + 1;
        ++lineNumber;
    }
    if (result.insertions.empty()) {
        result.text.clear();
        return result;
    }
    result.text.append(text.substr(copiedUpTo));
    return result;
}

TextPosition to_original(std::span<const Insertion> insertions, TextPosition position) {
    // An insertion moves what follows it on its line one code unit to the right; the `;` itself
    // maps to where it went in, the end of the name.
    int shift { 0 };
    for (const auto& insertion : insertions) {
        if (insertion.line == position.line && position.character > insertion.character) ++shift;
    }
    return TextPosition { position.line, position.character - shift };
}

namespace {

// Whether `line`, without its leading blanks, is an import or module directive (P1857: `export` may come first).
bool directive_line(std::string_view line) {
    std::size_t at { skip_blanks(line, 0) };
    if (keyword_at(line, at, "export")) at = skip_blanks(line, at + 6);
    return keyword_at(line, at, "import") || keyword_at(line, at, "module");
}

} // namespace

std::optional<LineRange> directive_missing_semicolon(std::string_view text, int line) {
    const auto lines = base::split_lines(text);
    if (line < 0 || lines.empty()) return std::nullopt;
    // The directive is above the line the diagnostic is on; on that line itself, the diagnostic is already right.
    for (int at { std::min(line - 1, static_cast<int>(lines.size()) - 1) }; at >= 0; --at) {
        std::string_view current { lines[static_cast<std::size_t>(at)] };
        // A line comment after the directive is not part of it.
        if (const std::size_t comment { current.find("//") }; comment != std::string_view::npos) current = current.substr(0, comment);
        const std::string_view trimmed { base::trim(current) };
        if (trimmed.empty()) continue;   // blank, or only a comment: look further up
        if (!directive_line(current) || trimmed.ends_with(';')) return std::nullopt;
        const std::size_t end { current.find_last_not_of(" \t\r") + 1 };
        const int endCharacter { static_cast<int>(base::utf16_length(current.substr(0, end))) };
        return LineRange { at, std::max(0, endCharacter - 1), endCharacter };
    }
    return std::nullopt;
}

std::optional<std::string> module_not_found_name(std::string_view message) {
    static constexpr std::string_view TAIL { "' not found" };
    if (message.size() < 8 || (message[0] != 'm' && message[0] != 'M') || !message.substr(1).starts_with("odule '") || !message.ends_with(TAIL)) return std::nullopt;
    const std::string_view name { message.substr(8, message.size() - 8 - TAIL.size()) };
    if (name.empty() || name.find('\'') != std::string_view::npos) return std::nullopt;
    return std::string { name };
}

} // namespace mcppls::engine::clangd
