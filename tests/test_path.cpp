import std;
import mcppls.testing;
import mcppls.base.path;

using namespace mcppls::base;

int main() {
    using namespace mcppls::testing;
    constexpr auto WIN { PathStyle::windows };
    constexpr auto POSIX { PathStyle::posix };

    "POSIX normalization"_test = [] {
        expect(normalize_path("/a/./b/../c//d/", POSIX) == "/a/c/d");
        expect(normalize_path("a/../../b", POSIX) == "../b");
        expect(normalize_path("/../x", POSIX) == "/x");
        expect(is_absolute_path("/x", POSIX) && !is_absolute_path("x/y", POSIX));
        expect(join_path("/a/b", "../c.cppm", POSIX) == "/a/c.cppm");
        expect(join_path("/a", "/abs", POSIX) == "/abs");
        expect(parent_path("/a/b/c.cpp", POSIX) == "/a/b");
        expect(parent_path("/a", POSIX) == "/");
    };

    "Windows normalization"_test = [] {
        expect(normalize_path("c:\\Users\\x\\..\\y\\m.cppm", WIN) == "C:/Users/y/m.cppm");
        expect(normalize_path("\\\\server\\share\\dir\\f", WIN) == "//server/share/dir/f");
        expect(is_absolute_path("C:\\x", WIN) && is_absolute_path("C:/x", WIN) && !is_absolute_path("C:x", WIN));
        expect(parent_path("C:/a", WIN) == "C:/");
        expect(join_path("C:/a/b", "..\\c", WIN) == "C:/a/c");
    };

    "names and extensions"_test = [] {
        expect(file_name("/a/b/greet.cppm") == "greet.cppm");
        expect(extension("/a/b/greet.cppm") == ".cppm");
        expect(extension("/a/.hidden").empty());
        expect(extension("/a/b.d/noext").empty());
    };

    "containment and relative paths"_test = [] {
        expect(is_within("/p/src/a.cpp", "/p", false));
        expect(!is_within("/p2/a.cpp", "/p", false));
        expect(is_within("C:/P/src", "c:/p", true));
        expect(relative_path("/p/src/a.cpp", "/p", POSIX) == std::optional<std::string> { "src/a.cpp" });
        expect(!relative_path("/q/a.cpp", "/p", POSIX).has_value());
        expect(path_key("C:/A", true) == "c:/a");
        expect(same_path("/a/B", "/a/B", false) && !same_path("/a/B", "/a/b", false));
    };

    return report();
}
