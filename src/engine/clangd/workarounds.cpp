module mcppls.engine.clangd.workarounds;

import std;
import mcppls.base.text;

namespace mcppls::engine::clangd {

namespace {

constexpr std::array<Workaround, 5> REGISTRY { {
    {
        .id = TRAILING_DOT_MODULE_NAME,
        .title = "a module name ending in '.' at the end of its line spins clangd forever; clangd is given the line with ';' after the dot",
        .fixedIn = "",
        .upstream = "unfiled; fixed on llvm-project main, likely by 6dcfc17b1b (#187846), not in 23.1.2",
        .evidence = ".agents/docs/2026-09-25-import-hang-status-highlight.md §1; conformance fixtures typing-import, workaround-canaries",
        .added = "0.0.4",
        .removeWhen = "the bundled clangd and the oldest clangd the server supports finish `import a.` at once",
        .canary = "conformance/fixtures/workaround-canaries: clangd --check on `import hello.` does not finish",
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
            if (const std::size_t point { insertion_point(line) }; point != std::string_view::npos) {
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

} // namespace mcppls::engine::clangd
