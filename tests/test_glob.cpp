// LSP glob patterns (S2 watch entries).
import std;
import mcppls.testing;
import mcppls.base.glob;

using mcppls::base::glob_match;

int main() {
    using namespace mcppls::testing;

    "segments, stars and double stars"_test = [] {
        expect(glob_match("mcpp.toml", "mcpp.toml", false));
        expect(!glob_match("mcpp.toml", "sub/mcpp.toml", false)) << "a plain name matches at the root only";
        expect(glob_match("**/mcpp.toml", "mcpp.toml", false) && glob_match("**/mcpp.toml", "a/b/mcpp.toml", false));
        expect(glob_match("src/**/*.cppm", "src/a.cppm", false) && glob_match("src/**/*.cppm", "src/x/y/a.cppm", false));
        expect(!glob_match("src/**/*.cppm", "lib/a.cppm", false) && !glob_match("src/*.cppm", "src/x/a.cppm", false)) << "a star stays in its segment";
        expect(glob_match("*", "file", false) && !glob_match("*", "dir/file", false));
    };

    "braces, question marks and classes"_test = [] {
        expect(glob_match("**/*.{cppm,ixx}", "a/b.ixx", false) && !glob_match("**/*.{cppm,ixx}", "a/b.cpp", false));
        expect(glob_match("{src,lib}/**/*.c{pp,c}", "lib/x.cc", false));
        expect(glob_match("file.?pp", "file.cpp", false) && !glob_match("file.?pp", "file.pp", false));
        expect(glob_match("example.[0-9]", "example.7", false) && !glob_match("example.[0-9]", "example.a", false));
        expect(glob_match("example.[!0-9]", "example.a", false) && !glob_match("example.[!0-9]", "example.7", false));
    };

    "case follows the file system"_test = [] {
        expect(!glob_match("SRC/*.CPPM", "src/a.cppm", false));
        expect(glob_match("SRC/*.CPPM", "src/a.cppm", true));
    };

    return report();
}
