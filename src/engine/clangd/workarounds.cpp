module mcppls.engine.clangd.workarounds;

import std;
import mcppls.base.text;

namespace mcppls::engine::clangd {

namespace {

constexpr std::array<Workaround, 12> REGISTRY { {
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
    {
        .id = BACKGROUND_INDEX_WITHOUT_MODULES,
        .title = "clangd's background index compiles a module unit without building its modules, so a definition in an implementation unit is indexed apart from its declaration or not at all; the server builds implementation units through clangd's foreground",
        .fixedIn = "",
        .upstream = "unfiled; the symptoms of clangd/clangd#2569 (references and rename inside modules)",
        .evidence = ".agents/docs/2026-09-27-qt-demo-navigation-discovery-plan.md §2.3 (BackgroundIndex::index has no ModulesBuilder); conformance fixture mcpp-partition-definition",
        .added = "0.0.6",
        .removeWhen = "clangd's background index builds the modules a unit imports before indexing it",
        .canary = "",
        .premise = "a unit clangd has built in the foreground keeps its symbols in clangd's index after it is closed",
    },
    {
        .id = MODULE_SCAN_PER_REQUEST,
        .title = "with --experimental-modules-support clangd scans a file's module dependencies again for every completion, about 170 ms more on a heavy header; a project that uses no modules gets clangd without it",
        .fixedIn = "",
        .upstream = "unfiled (UP-23 in issue #24)",
        .evidence = ".agents/docs/reviews/2026-10-01-issue-37-review.md §B (vulkan-rt, issue #37): completion median 82 ms without the flag, 254 ms with it (max 869 ms), in a .cpp and a header alike",
        .added = "0.0.9",
        .removeWhen = "clangd reuses a file's module dependency scan between requests, so completion costs the same with and without --experimental-modules-support",
        .canary = "",
        .premise = "a project whose plan has no module unit, no module import and no standard library module needs nothing of clangd's modules support; a plan that gains one restarts clangd with it",
    },
    {
        .id = CONST_CORRECTNESS_VIEWS,
        .title = "clang-tidy 23.1's misc-const-correctness says a variable holding a filter, drop_while, chunk_by or split view, or a view over one, can be const although such a view cannot be iterated as const; the diagnostic is dropped",
        .fixedIn = "",
        .upstream = "unfiled (UP-22 in issue #24); clang-tidy 22.1.8 does not warn for a view an adaptor returned",
        .evidence = "issue #37 (vulkan-rt rank_device_by_memory); .agents/docs/reviews/2026-10-01-issue-37-review.md §A; tests/test_workarounds.cpp",
        .added = "0.0.9",
        .removeWhen = "misc-const-correctness leaves a variable alone whose view has no const begin() and is used through it",
        .canary = "",
        .premise = "the diagnostic names the variable's type, the canonical one after `aka` where it differs, and that type is a std::ranges view whose base is its first template argument",
    },
    {
        .id = LEFT_BEHIND_MODULE_COPIES,
        .title = "clangd leaves the copy-on-read BMI it hands a reader behind when it dies; mcppls removes the previous clangd's leftovers before starting the next one",
        .fixedIn = "",
        .upstream = "#24 UP-24 (unfiled); the GC from llvm/llvm-project#193973 (3-day atime threshold) is in 23.1.0, the leak is not",
        .evidence = ".agents/reviews/mcppls-cache-20261002-cause-analysis.md; .agents/docs/2026-10-02-cache-growth-root-fix-plan.md C-7; conformance fixtures cache-budget, workaround-canaries",
        .added = "0.0.10",
        .removeWhen = "the bundled clangd leaves no copy-on-read file behind after a process dies, or removes an earlier clangd's leftovers of the same cache root within minutes",
        .canary = "conformance/fixtures/cache-budget: copies a dead generation left are gone after the next start, while the published BMIs stay",
        .premise = "the cache directory under <cdb>/.cache/clangd belongs to this server while it holds the workspace lease, so anything the previous clangd left there and no reader holds may be removed (the same premise as RD12, which clears the module locks there)",
    },
    {
        .id = MODULE_IMPORTER_COMPLETION_BUDGET,
        .title = "clangd 23.1 answers a module importer's completions in about a second, just past the budget; on such a file the budget waits up to 2.5 s for those answers instead of cancelling each at the line",
        .fixedIn = "",
        .upstream = "unfiled (UP-25 in issue #24): the module context is re-loaded per request, so a file that imports modules pays it on every completion while a file without imports answers in 60 ms",
        .evidence = "LSP replay probe on the qt-demo CDB, 2026-10-04: 950-1050 ms per completion on the importer at every position, 60 ms on a file without imports of the same project; tests/test_completion.cpp (EnginePace); conformance fixtures completion-keywords, ux-xlings, ux-mcpp, ux-heavy-headers",
        .added = "0.0.11",
        .removeWhen = "clangd keeps a file's module context between requests, or carries the imports in its preamble, so a module importer's completion costs no more than another file's",
        .canary = "",
        .premise = "the engine's answers for one file arrive reliably and just past the flat budget: two answers in the last sixty seconds, all within two seconds of the ask and all past the flat budget, extend that file's budget to their slowest plus 500 ms (2.5 s at most). Answers far beyond that are not counted -- a busy engine is not a slow-and-steady one -- and an engine that answers rarely (a broken module rebuilding, a fan-out save) or quickly keeps the flat budget, and the fallback with it",
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

namespace {

// The standard views without a const begin(): each caches the begin() it found, so iterating one changes it.
constexpr std::array<std::string_view, 4> NON_CONST_ITERABLE_VIEWS { "filter_view", "drop_while_view", "chunk_by_view", "split_view" };

struct ViewType {
    std::string_view name;   // "filter_view"
    std::string_view base;   // its first template argument: the range it is built on
};

// "std::ranges::filter_view<V, P>" (or "ranges::", libc++'s "std::__1::ranges::", the bare name an `aka` may print)
// -> {"filter_view", "V"}; anything that is not a standard range view -> nullopt.
std::optional<ViewType> view_type(std::string_view type) {
    type = base::trim(type);
    const std::size_t open { type.find('<') };
    if (open == std::string_view::npos) return std::nullopt;
    const std::string_view head { type.substr(0, open) };
    std::string_view name { head };
    if (const std::size_t colons { head.rfind("::") }; colons != std::string_view::npos) {
        const std::string_view scope { head.substr(0, colons + 2) };
        if (scope != "std::ranges::" && scope != "ranges::" && scope != "std::__1::ranges::") return std::nullopt;
        name = head.substr(colons + 2);
    }
    if (!name.ends_with("_view")) return std::nullopt;
    // The first argument ends at a ',' or the closing '>' outside nested <>, () and [] -- a lambda prints as
    // "(lambda at f.cpp:3:5)", a function pointer as "bool (*)(const H &)".
    int depth { 0 };
    std::size_t at { open + 1 };
    for (; at < type.size(); ++at) {
        const char c { type[at] };
        if (c == '<' || c == '(' || c == '[') {
            ++depth;
        } else if (c == '>' || c == ')' || c == ']') {
            if (depth == 0) break;
            --depth;
        } else if (c == ',' && depth == 0) {
            break;
        }
    }
    if (at >= type.size()) return std::nullopt;
    return ViewType { name, base::trim(type.substr(open + 1, at - open - 1)) };
}

} // namespace

bool const_correctness_on_non_const_view(std::string_view message) {
    // clang-tidy: "variable 'v' of type 'T' can be declared 'const'", where T is followed by " (aka 'U')" when its
    // canonical type U is spelled otherwise; clangd capitalises the first letter.
    static constexpr std::string_view TAIL { " can be declared 'const'" };
    const std::size_t tail { message.find(TAIL) };
    if (tail == std::string_view::npos || message.size() < 10 || (message[0] != 'V' && message[0] != 'v') || !message.substr(1).starts_with("ariable '")) return false;
    const std::string_view head { message.substr(0, tail) };
    std::string_view type;
    if (const std::size_t aka { head.rfind(" (aka '") }; aka != std::string_view::npos && head.ends_with("')")) {
        type = head.substr(aka + 7, head.size() - aka - 9);
    } else if (const std::size_t of { head.find(" of type '") }; of != std::string_view::npos && head.ends_with('\'')) {
        type = head.substr(of + 10, head.size() - of - 11);
    } else {
        return false;
    }
    // Down the views each is built on: a view over one without a const begin() has none either (its const begin()
    // asks for a range<const V>), except ref_view, whose const begin() reaches the range it refers to as it is.
    for (int depth { 0 }; depth < 16; ++depth) {
        const auto view = view_type(type);
        if (!view || view->name == "ref_view") return false;
        if (std::ranges::find(NON_CONST_ITERABLE_VIEWS, view->name) != NON_CONST_ITERABLE_VIEWS.end()) return true;
        type = view->base;
    }
    return false;
}

std::optional<std::string> module_not_found_name(std::string_view message) {
    static constexpr std::string_view TAIL { "' not found" };
    if (message.size() < 8 || (message[0] != 'm' && message[0] != 'M') || !message.substr(1).starts_with("odule '") || !message.ends_with(TAIL)) return std::nullopt;
    const std::string_view name { message.substr(8, message.size() - 8 - TAIL.size()) };
    if (name.empty() || name.find('\'') != std::string_view::npos) return std::nullopt;
    return std::string { name };
}

} // namespace mcppls::engine::clangd
