import std;
import mcppls.testing;
import mcppls.os;
import mcppls.base.path;
import mcppls.base.uri;

using namespace mcppls::base;

int main() {
    using namespace mcppls::testing;

    "POSIX URIs round-trip with percent encoding"_test = [] {
        expect(uri_to_path("file:///home/u/a%20b.cpp", PathStyle::posix).value_or("") == "/home/u/a b.cpp");
        expect(path_to_uri("/home/u/a b.cpp", PathStyle::posix) == "file:///home/u/a%20b.cpp");
        expect(path_to_uri("/x/c++/m.cppm", PathStyle::posix) == "file:///x/c%2B%2B/m.cppm");
        expect(uri_to_path("file:///x/c%2B%2B/m.cppm", PathStyle::posix).value_or("") == "/x/c++/m.cppm");
    };

    "VS Code Windows URIs"_test = [] {
        expect(uri_to_path("file:///c%3A/Users/x/m.cppm", PathStyle::windows).value_or("") == "C:/Users/x/m.cppm");
        expect(uri_to_path("file:///C:/Users/x/m.cppm", PathStyle::windows).value_or("") == "C:/Users/x/m.cppm");
        expect(path_to_uri("C:\\Users\\x\\m.cppm", PathStyle::windows) == "file:///c%3A/Users/x/m.cppm");
        expect(uri_to_path("file://server/share/f.cpp", PathStyle::windows).value_or("") == "//server/share/f.cpp");
    };

    "non-file URIs are errors"_test = [] {
        expect(!uri_to_path("untitled:Untitled-1").has_value());
        expect(!uri_to_path("https://example.com/a").has_value());
    };

    "native style follows the target"_test = [] {
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
            expect(NATIVE_PATH_STYLE == PathStyle::windows);
        } else {
            expect(NATIVE_PATH_STYLE == PathStyle::posix);
        }
    };

    return report();
}
