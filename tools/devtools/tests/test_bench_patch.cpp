// The unified-diff fallback applier (bench_patch.cppm), exercised directly so it does not need
// git on PATH -- see run.py's module docstring for the a/a/, b/b/ path convention this expects.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.devtools.bench.patch;

namespace bench = mcppls::devtools::bench;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

std::string temp_dir(std::string_view name) {
    const auto root = std::filesystem::current_path() / ".test-scratch"
                      / std::format("bench-patch-{}-{}", name, std::random_device {}());
    std::filesystem::create_directories(root);
    return root.string();
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "a one-line change applies, a/a b/b prefixes stripped by -p2"_test = [&] {
        const std::string dir { temp_dir("rename") };
        (void) fs::write_file(base::join_path(dir, "greet.txt"), "hello\nworld\n");
        const std::string patch {
            "diff --git a/a/greet.txt b/b/greet.txt\n"
            "index 0000000..1111111 100644\n"
            "--- a/a/greet.txt\n"
            "+++ b/b/greet.txt\n"
            "@@ -1,2 +1,2 @@\n"
            "-hello\n"
            "+salute\n"
            " world\n"
        };
        auto applied = bench::apply_unified_diff(patch, dir);
        expect(applied.has_value()) << (applied ? "" : applied.error().message);
        auto content = fs::read_file(base::join_path(dir, "greet.txt"));
        expect(content.has_value());
        expect(content.has_value() && *content == "salute\nworld\n");
        std::filesystem::remove_all(dir);
    };

    "a hunk that creates a new file (old side /dev/null) writes it"_test = [&] {
        const std::string dir { temp_dir("newfile") };
        const std::string patch {
            "diff --git a/a/new.txt b/b/new.txt\n"
            "new file mode 100644\n"
            "index 0000000..1111111\n"
            "--- /dev/null\n"
            "+++ b/b/new.txt\n"
            "@@ -0,0 +1,2 @@\n"
            "+line one\n"
            "+line two\n"
        };
        auto applied = bench::apply_unified_diff(patch, dir);
        expect(applied.has_value()) << (applied ? "" : applied.error().message);
        auto content = fs::read_file(base::join_path(dir, "new.txt"));
        expect(content.has_value() && *content == "line one\nline two\n");
        std::filesystem::remove_all(dir);
    };

    "a hunk that deletes a file (new side /dev/null) removes it"_test = [&] {
        const std::string dir { temp_dir("delfile") };
        (void) fs::write_file(base::join_path(dir, "gone.txt"), "bye\n");
        const std::string patch {
            "diff --git a/a/gone.txt b/b/gone.txt\n"
            "deleted file mode 100644\n"
            "index 1111111..0000000\n"
            "--- a/a/gone.txt\n"
            "+++ /dev/null\n"
            "@@ -1 +0,0 @@\n"
            "-bye\n"
        };
        auto applied = bench::apply_unified_diff(patch, dir);
        expect(applied.has_value()) << (applied ? "" : applied.error().message);
        expect(!fs::exists(base::join_path(dir, "gone.txt")));
        std::filesystem::remove_all(dir);
    };

    "context that does not match the file on disk is a hard failure"_test = [&] {
        const std::string dir { temp_dir("mismatch") };
        (void) fs::write_file(base::join_path(dir, "f.txt"), "totally different\n");
        const std::string patch {
            "diff --git a/a/f.txt b/b/f.txt\n"
            "--- a/a/f.txt\n"
            "+++ b/b/f.txt\n"
            "@@ -1 +1 @@\n"
            "-hello\n"
            "+salute\n"
        };
        auto applied = bench::apply_unified_diff(patch, dir);
        expect(!applied.has_value());
        std::filesystem::remove_all(dir);
    };

    "apply_patch (the full pipeline) applies the same fallback when the diff has no /dev/null lines"_test = [&] {
        const std::string dir { temp_dir("pipeline") };
        (void) fs::write_file(base::join_path(dir, "src.txt"), "alpha\nbeta\n");
        const std::string patchPath { base::join_path(dir, "the.patch") };
        (void) fs::write_file(patchPath,
            "diff --git a/a/src.txt b/b/src.txt\n"
            "--- a/a/src.txt\n"
            "+++ b/b/src.txt\n"
            "@@ -1,2 +1,2 @@\n"
            "-alpha\n"
            "+ALPHA\n"
            " beta\n");
        auto applied = bench::apply_patch(patchPath, dir);
        expect(applied.has_value()) << (applied ? "" : applied.error().message);
        auto content = fs::read_file(base::join_path(dir, "src.txt"));
        expect(content.has_value() && *content == "ALPHA\nbeta\n");
        std::filesystem::remove_all(dir);
    };

    return report();
}
