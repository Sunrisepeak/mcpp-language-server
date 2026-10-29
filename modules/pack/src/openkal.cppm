// The openkal kit (S4, kit-version 2; ecosystem design 2026-10-29 §9.2): the semantic kit a payload
// carries, which reads code for any system with the standard library and no system headers.
//
// It is assembled from what mcpp compiles an ok image with, not from a recipe of its own: for each
// target (`x86_64-openkal`, `aarch64-openkal`) mcpp's build database (`mcpp emit build-database
// --spec compile-commands --target <t>`) names the include directories, the defines and the triple
// of a unit of the project, and the std module sources of the platform's C++ run-time. The kit
// copies those directories once -- a directory both targets read is shared, one only a target reads
// goes under `include/<target>/` -- and writes a module manifest for `std` and `std.compat` and a
// kit.json whose `targets` give each target its include directories in its own order (a C
// library's headers of an architecture come before its generic ones).
//
// Left out: a run-time's own sources (`.../src`, LLVM libc's internals), which a unit includes only
// to build the run-time itself.
export module mcppls.pack.openkal;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::pack::openkal {

// What a build database says of one target: the platform's system include directories, as the
// std module of its C++ run-time is compiled with them, and how a unit of the project reads.
struct TargetCommand {
    std::string name;                     // x86_64-openkal
    std::vector<std::string> arguments;   // a unit's compiler arguments, without the compiler and the source
    std::string stdModule;                // the std module source the build compiles (std.cppm)
    std::string stdCompatModule;          // std.compat.cppm; empty when the build names none and none lies beside std.cppm
    std::vector<std::string> includes;    // the std module command's -isystem directories, in order
    std::vector<std::string> stdIncludes; // its -I directories (where its `std/*.inc` are)
};

// The command of `name` read from the compile commands `database` describes: the first unit whose
// file is under `projectRoot`, and the std module's command.
base::Result<TargetCommand> command_of(std::string_view name, const nlohmann::json& database, std::string_view projectRoot);

// Where each directory goes and what each target reads, decided from the commands alone.
struct Copy {
    std::string source;        // absolute
    std::string destination;   // relative to the kit root
};
struct TargetLayout {
    std::string name;                     // x86_64-openkal
    std::string triple;                   // the engine's --target: x86_64-unknown-none-elf
    std::vector<std::string> includes;    // kit paths, in the build's order
    std::vector<std::string> arguments;   // what decides how a unit reads: defines, -fno-short-wchar, -nostd*
};
struct Layout {
    std::vector<Copy> copies;
    std::vector<TargetLayout> targets;
};
base::Result<Layout> plan(std::span<const TargetCommand> commands);

// The libc++ version the headers in `includes` carry: `_LIBCPP_VERSION` of their `__config`
// (220108 is 22.1.8). Nullopt when none of them has one.
std::optional<std::string> libcxx_version(std::span<const std::string> includes);

struct Options {
    std::vector<TargetCommand> commands;
    std::string outDir;          // replaced if it exists
    std::string version;         // the platform's version, which the kit's name and package version carry
};

struct BuiltKit {
    std::string kitDir;
    std::string name;
    std::string libcxxVersion;
    std::uint64_t fileCount { 0 };
    std::uint64_t totalBytes { 0 };
};

// Copies the directories, the std module sources and the license texts, writes the module manifest
// and kit.json.
base::Result<BuiltKit> build(const Options& options);

} // namespace mcppls::pack::openkal
