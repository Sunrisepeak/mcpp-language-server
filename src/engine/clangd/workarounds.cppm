// What the server does only because of a defect in a clangd it drives (import-hang plan §9): one
// registry, one entry per workaround, each saying which versions need it, the upstream defect,
// the evidence, when it can go and the canary that says so. The engine's traits are read from
// here, so a workaround is on exactly where its entry says, and nowhere else. Code that carries
// one out names its id (`WA-CLANGD-001`) at the call site.
export module mcppls.engine.clangd.workarounds;

import std;

export namespace mcppls::engine::clangd {

// The clangd release line the payload pins and the conformance suite runs. A version of this line
// needs a workaround until the entry's `fixedIn`; a version of any other line has not been seen
// by the suite, so it gets every workaround.
inline constexpr std::string_view KNOWN_LINE { "23.1" };

struct Workaround {
    std::string_view id;           // WA-CLANGD-<n>: grep-able, in logs and the report
    std::string_view title;        // what it works around, in one line
    std::string_view fixedIn;      // the first release of KNOWN_LINE that no longer needs it; empty: none yet
    std::string_view upstream;     // the upstream issue or fix, or "unfiled"
    std::string_view evidence;     // where it was found and measured
    std::string_view added;        // the mcppls version that added it
    std::string_view removeWhen;   // when the workaround can go
    std::string_view canary;       // the check that fails once the defect is gone; empty: none yet
};

inline constexpr std::string_view TRAILING_DOT_MODULE_NAME { "WA-CLANGD-001" };
inline constexpr std::string_view UNRESOLVED_IMPORT_STAND_INS { "WA-CLANGD-002" };
inline constexpr std::string_view MODULE_PREPARATION { "WA-CLANGD-003" };
inline constexpr std::string_view MODULE_HINTS { "WA-CLANGD-004" };
inline constexpr std::string_view MSVC_STL_ALIGNED_ALLOCATION { "WA-CLANGD-005" };

std::span<const Workaround> workarounds();
const Workaround* find_workaround(std::string_view id);
// Whether clangd `version` needs `workaround`.
bool needs(const Workaround& workaround, std::string_view version);
bool needs(std::string_view id, std::string_view version);
// The ids of every workaround clangd `version` needs, in registry order.
std::vector<std::string_view> active_workarounds(std::string_view version);

// WA-CLANGD-001. clangd 23.1 never finishes a file in which a module name of an `import` or
// `module` directive ends in `.` with nothing after the dot on its line but white space or a
// comment: the build spins at a full core, and every later version of the file queues behind it.
// Such a directive is always an error (P1857: the directive ends with its line), and so is the
// same line with `;` right after the dot, which clangd reports at once. `text` is what clangd is
// given instead; each insertion is where a `;` went in, in the rewritten text's coordinates.
struct Insertion {
    int line { 0 };
    int character { 0 };   // UTF-16, like every LSP position
};

struct Sanitized {
    std::string text;
    std::vector<Insertion> insertions;
    bool changed() const { return !insertions.empty(); }
};

Sanitized sanitize_module_names(std::string_view text);

// A position clangd reported in the rewritten text, in the text the editor has.
struct TextPosition {
    int line { 0 };
    int character { 0 };
};
TextPosition to_original(std::span<const Insertion> insertions, TextPosition position);

} // namespace mcppls::engine::clangd
