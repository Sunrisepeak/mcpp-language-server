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
    bool outerIsCMake_ { false };
    std::vector<std::string> members_;   // an mcpp workspace's members, relative, as written
};

// The nearest directory at or above `directory` with a build manifest; `directory` itself when none has.
std::string enclosing_project_root(std::string_view directory);

// The quoted entries of `[workspace] members = [ ... ]` in an mcpp.toml's text.
std::vector<std::string> workspace_members(std::string_view manifestText);

} // namespace mcppls::project
