import std;
import mcppls.testing;
import mcppls.base.path;
import mcpplibs.os;

namespace os = mcpplibs::os;
namespace base = mcppls::base;

int main() {
    std::error_code ignored;
    using namespace mcppls::testing;
    const std::string root { base::join_path(base::spelled(os::dirs::temp()),
        std::format("mcppls-test-fs-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };

    "temporary directory is absolute"_test = [&] {
        expect(base::is_absolute_path(root)) << root;
    };

    "nested directories are created"_test = [&] {
        std::error_code ignored;
        expect(fatal([&] { std::error_code failed; std::filesystem::create_directories(base::join_path(root, "a/b/c"), failed); return !failed; }()));
        expect(std::filesystem::is_directory(base::join_path(root, "a/b/c"), ignored));
        expect([&] { std::error_code failed; std::filesystem::create_directories(base::join_path(root, "a/b/c"), failed); return !failed; }());
    };

    "a small file is written and read"_test = [&] {
        std::error_code ignored;
        const std::string file { base::join_path(root, "a/small.txt") };
        expect(fatal(os::fs::write(file, "first").has_value()));
        expect(os::fs::read(file).value_or("") == "first");
        expect(std::filesystem::is_regular_file(file, ignored));
        expect(!std::filesystem::is_directory(file, ignored));
    };

    "an atomic write creates a file"_test = [&] {
        const std::string file { base::join_path(root, "a/data.json") };
        expect(fatal(os::fs::write_atomic(file, "first").has_value()));
        expect(os::fs::read(file).value_or("") == "first");
    };

    "an atomic write replaces an existing file"_test = [&] {
        const std::string file { base::join_path(root, "a/data.json") };
        expect(fatal(os::fs::write_atomic(file, "second").has_value()));
        expect(os::fs::read(file).value_or("") == "second");
    };

    "a large file is written in one call"_test = [&] {
        const std::string file { base::join_path(root, "a/large.bin") };
        expect(fatal(os::fs::write(file, std::string(100000, 'x')).has_value()));
        auto content = os::fs::read(file);
        expect(content.has_value() && content->size() == 100000u);
    };

    "a large atomic write replaces an existing file"_test = [&] {
        const std::string file { base::join_path(root, "a/data.json") };
        expect(fatal(os::fs::write_atomic(file, std::string(100000, 'y')).has_value()));
    };

    "the large replacement reads back"_test = [&] {
        auto content = os::fs::read(base::join_path(root, "a/data.json"));
        expect(content.has_value() && content->size() == 100000u);
    };

    "a directory lists its children and no temporary file"_test = [&] {
        std::vector<std::string> names;
        for (const auto& entry : base::children(base::join_path(root, "a"))) names.emplace_back(base::file_name(entry));
        const std::vector<std::string> expected { "b", "data.json", "large.bin", "small.txt" };
        expect(names == expected) << std::format("{}", names);
    };

    "binary content survives"_test = [&] {
        const std::string file { base::join_path(root, "binary.bin") };
        std::string bytes;
        for (int i { 0 }; i < 256; ++i) bytes.push_back(static_cast<char>(i));
        bytes += "\r\n\n\r";
        expect(fatal(os::fs::write(file, bytes).has_value()));
        expect(os::fs::read(file).value_or("") == bytes);
    };

    "stamp changes when content changes"_test = [&] {
        const std::string file { base::join_path(root, "stamp.txt") };
        expect(os::fs::write(file, "1").has_value());
        const auto first = base::file_stamp(file);
        expect(fatal(first.has_value()));
        expect(os::fs::write(file, "22").has_value());
        const auto second = base::file_stamp(file);
        expect(fatal(second.has_value()));
        expect(second->size == 2u);
        expect(*first != *second);
        expect(!base::file_stamp(base::join_path(root, "absent")).has_value());
    };

    "listing follows the skip rules"_test = [&] {
        for (std::string_view name : { "src/m.cppm", "src/main.cpp", "src/notes.txt", "target/x.cppm",
                                       ".git/y.cpp", "sub/node_modules/z.cpp", "sub/deep/w.CPP" }) {
            const std::string file { base::join_path(root, base::join_path("tree", name)) };
            expect([&] { std::error_code failed; std::filesystem::create_directories(base::parent_path(file), failed); return !failed; }());
            expect(os::fs::write(file, "x").has_value());
        }
        const std::array<std::string_view, 3> extensions { ".cpp", ".cppm", ".ixx" };
        const std::array<std::string_view, 2> skip { "target", "node_modules" };
        const auto files = base::files_under(base::join_path(root, "tree"), extensions, skip);
        std::vector<std::string> relative;
        for (const auto& file : files) relative.push_back(base::relative_path(file, base::join_path(root, "tree")).value_or(file));
        const std::vector<std::string> expected { "src/m.cppm", "src/main.cpp", "sub/deep/w.CPP" };
        expect(relative == expected) << std::format("{}", relative);
    };

    "missing files are errors, not exceptions"_test = [&] {
        std::error_code ignored;
        expect(!os::fs::read(base::join_path(root, "missing/file")).has_value());
        expect(!std::filesystem::exists(base::join_path(root, "missing"), ignored));
    };

    "a file has one identity whatever it is called, and another file another"_test = [&] {
        const std::string first { base::join_path(root, "identity/first.txt") };
        const std::string second { base::join_path(root, "identity/second.txt") };
        expect(fatal([&] { std::error_code failed; std::filesystem::create_directories(base::join_path(root, "identity"), failed); return !failed; }()));
        expect(fatal(os::fs::write(first, "1").has_value() && os::fs::write(second, "2").has_value()));
        // What the caches compare: an identity that is there or not.
        const auto identity = [](const std::string& path) -> std::optional<os::fs::identity> {
            auto found = os::fs::identity_of(path);
            return found ? std::optional { *found } : std::nullopt;
        };
        const auto a = identity(first);
        const auto b = identity(second);
        expect(fatal(a.has_value() && b.has_value()));
        expect(*a != *b);
        expect(identity(base::join_path(root, "identity/./first.txt")) == a);
        if (base::native_path_style() == base::PathStyle::windows) {
            std::string upper { first };
            std::ranges::transform(upper, upper.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
            expect(identity(upper) == a) << upper;
        }
        expect(!os::fs::identity_of(base::join_path(root, "identity/missing.txt")).has_value());
    };

    // Symbolic links need a privilege on Windows. What stands for them there is
    // the short alias a volume gives a long name: RUNNER~1 for runneradmin.
    "a file reached by another name has one canonical name"_test = [&] {
        std::error_code ignored;
        if (base::native_path_style() == base::PathStyle::windows) {
            expect(base::canonical_path("c:\\dir\\f.txt") == "C:/dir/f.txt") << base::canonical_path("c:\\dir\\f.txt");
            const std::string temporary { base::spelled(os::dirs::temp()) };
            std::string aliased;
            if (temporary.contains('~')) aliased = temporary;
            else if (std::filesystem::is_directory("C:/PROGRA~1", ignored)) aliased = "C:/PROGRA~1";
            if (!aliased.empty()) {
                const std::string resolved { base::canonical_path(aliased) };
                expect(!resolved.contains('~')) << aliased << " -> " << resolved;
                const auto same = os::fs::identity_of(resolved);
                const auto other = os::fs::identity_of(aliased);
                expect(same && other && *same == *other) << resolved;
                const std::string file { base::join_path(aliased, "mcppls-canonical-alias.txt") };
                if (os::fs::write(file, "x")) {
                    expect(base::canonical_path(file) == base::join_path(resolved, "mcppls-canonical-alias.txt")) << base::canonical_path(file);
                    std::filesystem::remove_all(file, ignored);
                }
            }
        } else {
            const std::string real { base::join_path(root, "canonical-real") };
            const std::string link { base::join_path(root, "canonical-link") };
            expect(fatal([&] { std::error_code failed; std::filesystem::create_directories(real, failed); return !failed; }()));
            expect(fatal(os::fs::write(base::join_path(real, "f.txt"), "x").has_value()));
            std::error_code error;
            std::filesystem::create_directory_symlink(std::filesystem::path { real }, std::filesystem::path { link }, error);
            expect(fatal(!error)) << error.message();
            const std::string throughLink { base::canonical_path(base::join_path(link, "f.txt")) };
            expect(throughLink == base::canonical_path(base::join_path(real, "f.txt"))) << throughLink;
            expect(!throughLink.contains("canonical-link")) << throughLink;
            expect(base::canonical_path(base::join_path(link, "missing/g.txt")).ends_with("canonical-real/missing/g.txt"))
                << base::canonical_path(base::join_path(link, "missing/g.txt"));
        }
    };

    "current directory is absolute"_test = [] {
        expect(base::is_absolute_path(base::current_directory())) << base::current_directory();
    };

    std::filesystem::remove_all(root, ignored);
    "removal is complete"_test = [&] {
        std::error_code ignored;
        expect(!std::filesystem::exists(root, ignored));
    };

    return report();
}
