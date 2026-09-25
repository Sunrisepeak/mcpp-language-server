// GCC and Clang dialect arguments translated for the Clang-based engine
// (design section 14.4, rows P1-P5), and engine commands built from a kit.
export module mcppls.normalize.gnu;

import std;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.toolchain.probe;

export namespace mcppls::normalize {

// The driver name written into engine commands when the build's own driver is
// not a Clang driver. The engine never executes it.
inline constexpr std::string_view ENGINE_CLANG_DRIVER { "clang++" };
// The same for a C source: the engine reads a source as C only when the driver is a C driver, since
// the translated arguments carry no -x.
inline constexpr std::string_view ENGINE_CLANG_C_DRIVER { "clang" };

// Every engine command stops before linking (issue #23, fix plan F1). The build's own `-c` goes with
// the rest of what only the build needs, and this one is added once: without it the driver plans a
// link, and a check made only for the link step -- `LTO requires -fuse-ld=lld` for windows-msvc with
// `-flto` -- makes clangd's module dependency scan fail, so no module is built. clangd's own
// `-fsyntax-only` takes precedence over it, so what clangd reads is unchanged; link arguments such as
// `-flto` stay, since they mean nothing once no link is planned.
inline constexpr std::string_view COMPILE_ONLY { "-c" };

struct GnuInput {
    std::span<const std::string> arguments;   // full compile command, argv[0] first, response files expanded
    std::string source;                       // absolute path of the unit's source
    std::string workDirectory;
    const toolchain::ToolchainFacts* facts { nullptr };
    bool importable { false };
    bool noAlignedAllocationWithMsvcStl { true };   // the core engine's trait (overall design 5.4)
};

// Arguments without argv[0], output, dependency files, BMI and scanning flags,
// the source file or -x, and with one COMPILE_ONLY; for GCC with target, standard library and
// installation made explicit.
std::vector<std::string> translate_gnu(const GnuInput& input);
// For a toolchain that targets the MSVC ABI: target, emulated cl version, the Visual
// Studio toolset and Windows SDK, and, when `noAlignedAllocation`, aligned allocation off with the
// MSVC STL (clangd 23.1.0 reports `align_val_t` as ambiguous inside its std module otherwise;
// usable plan E9). Arguments already in `existing` are not repeated.
std::vector<std::string> windows_msvc_arguments(const toolchain::ToolchainFacts& facts, std::span<const std::string> existing,
                                                bool noAlignedAllocation = true);
// Semantic arguments any dialect can keep: include paths, macros, standard, forced includes.
std::vector<std::string> semantic_subset(std::span<const std::string> arguments);
// Base arguments for a kit: target, standard, the kit's own arguments, include directories, sysroot,
// and COMPILE_ONLY.
std::vector<std::string> kit_arguments(const spec::Kit& kit, std::string_view languageStandard, std::string_view macosSdk);
// The -std= value in arguments, or empty.
std::string language_standard_of(std::span<const std::string> arguments);

} // namespace mcppls::normalize
