// Where one project ends and another, nested inside it, begins (real-project plan RP3.4). A
// directory below a project's root with a build manifest of its own is a separate project -- a
// conformance fixture, a vendored copy, an example -- unless it is part of the outer project's own
// build: a CMake subdirectory of a CMake project, or a member of an mcpp workspace. Scanning and a
// borrowed command never cross into a separate project.
export module mcppls.project.boundary;

import std;

export namespace mcppls::project {

class ProjectBoundaries {
public:
    // `outer` is the outer project's root: the directory its own manifest is in.
    explicit ProjectBoundaries(std::string_view outer);

    // Whether `directory`, strictly inside the outer root, is the root of a separate project.
    bool separate(std::string_view directory) const;
    // Whether some directory from `file`'s own up to (not including) `stop` is a separate project's root.
    bool crossed(std::string_view file, std::string_view stop) const;

private:
    std::string outer_;
    std::vector<std::string_view> outerNesting_;   // the nesting manifests the outer root itself has
    std::vector<std::string> members_;   // an mcpp workspace's members, relative, as written
};

// The files that make a directory a project's root, one list for every place that looks for one (plan
// 2026-09-27: xmake and meson joined): mcpp.toml, CMakeLists.txt, xmake.lua, meson.build and a bare
// compile_commands.json. The three of them that a build also puts in subdirectories -- CMakeLists.txt,
// xmake.lua, meson.build -- nest: a subdirectory's own is part of the project above it.
std::span<const std::string_view> project_manifests();
bool nesting_manifest(std::string_view manifest);

// The project root for `directory`: the nearest directory at or above it with a build manifest, and
// when that manifest nests, the outermost of the directories above it that carry the same one without a
// gap; `directory` itself when none has.
std::string enclosing_project_root(std::string_view directory);
// The same, or nullopt when no directory has a manifest.
std::optional<std::string> find_project_root(std::string_view directory);

// The quoted entries of `[workspace] members = [ ... ]` in an mcpp.toml's text.
std::vector<std::string> workspace_members(std::string_view manifestText);

} // namespace mcppls::project
