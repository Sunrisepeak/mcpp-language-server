// The series rule: a plugin of the server's series is served, one of another refused with both
// versions and the install that matches (docs/92-release.md "Versions").
import std;
import mcpplibs.mtest;
import mcppls.base.series;

using namespace mcpplibs::mtest;
namespace base = mcppls::base;

int main() {
    "the series of a three-part version"_test = [] {
        expect(base::series_of("0.21.3") == std::optional<std::string> { "0.21" });
        expect(base::series_of("1.2.0-rc1") == std::optional<std::string> { "1.2" });
        expect(!base::series_of("2026.11.8.1").has_value());
        expect(!base::series_of("0.21").has_value());
        expect(!base::series_of("dev").has_value());
    };
    "a plugin of the same series is served, whatever its patch"_test = [] {
        expect(!base::plugin_series_problem("zed", "0.21.4", "0.21.0").has_value());
    };
    "a plugin of another series is refused, naming both versions and the install"_test = [] {
        const auto why = base::plugin_series_problem("zed", "0.22.0", "0.21.1");
        expect(why.has_value());
        expect(why->contains("0.21.1") && why->contains("0.22.0") && why->contains("xlings install mcppls@0.22")) << *why;
    };
    "a version of another shape is not refused"_test = [] {
        expect(!base::plugin_series_problem("nvim", "dev", "0.21.0").has_value());
    };
    return report();
}
