import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;

namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

int main() {
    using namespace mcppls::testing;
    const std::string root { base::join_path(mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-fs-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };

    "temporary directory is absolute"_test = [&] {
        expect(base::is_absolute_path(root)) << root;
    };

    "nested directories are created"_test = [&] {
        expect(fatal(fs::create_directories(base::join_path(root, "a/b/c")).has_value()));
        expect(fs::is_directory(base::join_path(root, "a/b/c")));
        expect(fs::create_directories(base::join_path(root, "a/b/c")).has_value());
    };

    "a small file is written and read"_test = [&] {
        const std::string file { base::join_path(root, "a/small.txt") };
        expect(fatal(fs::write_file(file, "first").has_value()));
        expect(fs::read_file(file).value_or("") == "first");
        expect(fs::is_regular_file(file));
        expect(!fs::is_directory(file));
    };

    "an atomic write creates a file"_test = [&] {
        const std::string file { base::join_path(root, "a/data.json") };
        expect(fatal(fs::write_file_atomic(file, "first").has_value()));
        expect(fs::read_file(file).value_or("") == "first");
    };

    "an atomic write replaces an existing file"_test = [&] {
        const std::string file { base::join_path(root, "a/data.json") };
        expect(fatal(fs::write_file_atomic(file, "second").has_value()));
        expect(fs::read_file(file).value_or("") == "second");
    };

    "a large file is written in one call"_test = [&] {
        const std::string file { base::join_path(root, "a/large.bin") };
        expect(fatal(fs::write_file(file, std::string(100000, 'x')).has_value()));
        auto content = fs::read_file(file);
        expect(content.has_value() && content->size() == 100000u);
    };

    "a large atomic write replaces an existing file"_test = [&] {
        const std::string file { base::join_path(root, "a/data.json") };
        expect(fatal(fs::write_file_atomic(file, std::string(100000, 'y')).has_value()));
    };

    "the large replacement reads back"_test = [&] {
        auto content = fs::read_file(base::join_path(root, "a/data.json"));
        expect(content.has_value() && content->size() == 100000u);
    };

    "a directory lists its children and no temporary file"_test = [&] {
        std::vector<std::string> names;
        for (const auto& entry : fs::list_directory(base::join_path(root, "a"))) names.emplace_back(base::file_name(entry));
        const std::vector<std::string> expected { "b", "data.json", "large.bin", "small.txt" };
        expect(names == expected) << std::format("{}", names);
    };

    "binary content survives"_test = [&] {
        const std::string file { base::join_path(root, "binary.bin") };
        std::string bytes;
        for (int i { 0 }; i < 256; ++i) bytes.push_back(static_cast<char>(i));
        bytes += "\r\n\n\r";
        expect(fatal(fs::write_file(file, bytes).has_value()));
        expect(fs::read_file(file).value_or("") == bytes);
    };

    "stamp changes when content changes"_test = [&] {
        const std::string file { base::join_path(root, "stamp.txt") };
        expect(fs::write_file(file, "1").has_value());
        const auto first = fs::stamp(file);
        expect(fatal(first.has_value()));
        expect(fs::write_file(file, "22").has_value());
        const auto second = fs::stamp(file);
        expect(fatal(second.has_value()));
        expect(second->size == 2u);
        expect(*first != *second);
        expect(!fs::stamp(base::join_path(root, "absent")).has_value());
    };

    "listing follows the skip rules"_test = [&] {
        for (std::string_view name : { "src/m.cppm", "src/main.cpp", "src/notes.txt", "target/x.cppm",
                                       ".git/y.cpp", "sub/node_modules/z.cpp", "sub/deep/w.CPP" }) {
            const std::string file { base::join_path(root, base::join_path("tree", name)) };
            expect(fs::create_directories(base::parent_path(file)).has_value());
            expect(fs::write_file(file, "x").has_value());
        }
        const std::array<std::string_view, 3> extensions { ".cpp", ".cppm", ".ixx" };
        const std::array<std::string_view, 2> skip { "target", "node_modules" };
        const auto files = fs::list_files(base::join_path(root, "tree"), extensions, skip);
        std::vector<std::string> relative;
        for (const auto& file : files) relative.push_back(base::relative_path(file, base::join_path(root, "tree")).value_or(file));
        const std::vector<std::string> expected { "src/m.cppm", "src/main.cpp", "sub/deep/w.CPP" };
        expect(relative == expected) << std::format("{}", relative);
    };

    "missing files are errors, not exceptions"_test = [&] {
        expect(!fs::read_file(base::join_path(root, "missing/file")).has_value());
        expect(!fs::exists(base::join_path(root, "missing")));
    };

    "a file has one identity whatever it is called, and another file another"_test = [&] {
        const std::string first { base::join_path(root, "identity/first.txt") };
        const std::string second { base::join_path(root, "identity/second.txt") };
        expect(fatal(fs::create_directories(base::join_path(root, "identity")).has_value()));
        expect(fatal(fs::write_file(first, "1").has_value() && fs::write_file(second, "2").has_value()));
        const auto a = fs::file_identity(first);
        const auto b = fs::file_identity(second);
        expect(fatal(a.has_value() && b.has_value()));
        expect(*a != *b);
        expect(fs::file_identity(base::join_path(root, "identity/./first.txt")) == a);
        if constexpr (base::NATIVE_PATH_STYLE == base::PathStyle::windows) {
            std::string upper { first };
            std::ranges::transform(upper, upper.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
            expect(fs::file_identity(upper) == a) << upper;
        }
        expect(!fs::file_identity(base::join_path(root, "identity/missing.txt")).has_value());
    };

    // Symbolic links need a privilege on Windows. What stands for them there is
    // the short alias a volume gives a long name: RUNNER~1 for runneradmin.
    "a file reached by another name has one canonical name"_test = [&] {
        if constexpr (base::NATIVE_PATH_STYLE == base::PathStyle::windows) {
            expect(fs::canonical_path("c:\\dir\\f.txt") == "C:/dir/f.txt") << fs::canonical_path("c:\\dir\\f.txt");
            const std::string temporary { mcppls::platform::dirs::temp_directory() };
            std::string aliased;
            if (temporary.contains('~')) aliased = temporary;
            else if (fs::is_directory("C:/PROGRA~1")) aliased = "C:/PROGRA~1";
            if (!aliased.empty()) {
                const std::string resolved { fs::canonical_path(aliased) };
                expect(!resolved.contains('~')) << aliased << " -> " << resolved;
                expect(fs::file_identity(resolved) == fs::file_identity(aliased)) << resolved;
                const std::string file { base::join_path(aliased, "mcppls-canonical-alias.txt") };
                if (fs::write_file(file, "x")) {
                    expect(fs::canonical_path(file) == base::join_path(resolved, "mcppls-canonical-alias.txt")) << fs::canonical_path(file);
                    fs::remove_all(file);
                }
            }
        } else {
            const std::string real { base::join_path(root, "canonical-real") };
            const std::string link { base::join_path(root, "canonical-link") };
            expect(fatal(fs::create_directories(real).has_value()));
            expect(fatal(fs::write_file(base::join_path(real, "f.txt"), "x").has_value()));
            std::error_code error;
            std::filesystem::create_directory_symlink(std::filesystem::path { real }, std::filesystem::path { link }, error);
            expect(fatal(!error)) << error.message();
            const std::string throughLink { fs::canonical_path(base::join_path(link, "f.txt")) };
            expect(throughLink == fs::canonical_path(base::join_path(real, "f.txt"))) << throughLink;
            expect(!throughLink.contains("canonical-link")) << throughLink;
            expect(fs::canonical_path(base::join_path(link, "missing/g.txt")).ends_with("canonical-real/missing/g.txt"))
                << fs::canonical_path(base::join_path(link, "missing/g.txt"));
        }
    };

    "current directory is absolute"_test = [] {
        expect(base::is_absolute_path(fs::current_directory())) << fs::current_directory();
    };

    fs::remove_all(root);
    "removal is complete"_test = [&] { expect(!fs::exists(root)); };

    return report();
}
