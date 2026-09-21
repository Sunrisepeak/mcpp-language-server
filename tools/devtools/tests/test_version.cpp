// The version sites (.github/scripts/version.py, ported): a synthetic repository root with the
// minimal file set every site reads, so this does not depend on -- or risk writing into -- the
// real checkout.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.devtools.version;

namespace version = mcppls::devtools::version;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

std::string scratch(std::string_view name) {
    const std::string directory { base::join_path(
        mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-version-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void) fs::create_directories(directory);
    return directory;
}

void write(const std::string& path, std::string_view content) {
    (void) fs::create_directories(base::parent_path(path));
    (void) fs::write_file(path, content);
}

// A minimal root with every site version.py's --check reads, all agreeing on 2026.9.16.1.
std::string make_consistent_root() {
    const std::string root { scratch("root") };
    write(base::join_path(root, "mcpp.toml"), "[package]\nname = \"mcpp-language-server\"\nversion     = \"2026.9.16.1\"\n");
    write(base::join_path(root, "modules/base/src/version.cppm"),
          "export module mcppls.base.version;\n"
          "inline constexpr std::string_view VERSION { \"2026.9.16.1\" };\n"
          "inline constexpr std::string_view CLANGD_VERSION { \"23.1.0\" };\n"
          "inline constexpr std::string_view MINIMUM_MCPP_VERSION { \"2026.9.15.1\" };\n");
    write(base::join_path(root, "editors/vscode/package.json"), "{\n  \"name\": \"mcppls\",\n  \"version\": \"2026.916.1\"\n}\n");
    write(base::join_path(root, "editors/zed/extension.toml"), "id = \"mcppls\"\nversion = \"2026.916.1\"\n");
    write(base::join_path(root, "editors/clion/gradle.properties"), "pluginVersion = 2026.9.16.1\n");
    write(base::join_path(root, ".github/versions.env"), "MCPP_VERSION=2026.9.21.3\nLLVM_VERSION=22.1.8\n");
    write(base::join_path(root, "packaging/payload.lock.json"), "{\n  \"clangd-version\": \"23.1.0\"\n}\n");
    write(base::join_path(root, "src/spec/kit.cppm"), "inline constexpr int KIT_VERSION { 1 };\n");
    write(base::join_path(root, "modules/pack/src/kit.cppm"), "inline constexpr int KIT_VERSION { 1 };\n");
    return root;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "extension_version maps a date version, monotone within and across years"_test = [&] {
        auto a = version::extension_version("2026.9.16.1");
        expect(a.has_value() && *a == "2026.916.1");
        auto b = version::extension_version("2026.10.5.2");
        expect(b.has_value() && *b == "2026.1005.2");
        // 916 < 1005: monotone within the year without depending on lexical string order.
        expect(a.has_value() && b.has_value());
    };

    "extension_version leaves an already-three-part semantic version alone"_test = [&] {
        auto v = version::extension_version("1.2.3");
        expect(v.has_value() && *v == "1.2.3");
    };

    "extension_version rejects a shape that is neither"_test = [&] {
        auto v = version::extension_version("not-a-version");
        expect(!v.has_value());
    };

    "a consistent root has no problems, and --print/--print --extension agree"_test = [&] {
        const std::string root { make_consistent_root() };
        auto problems = version::check(root);
        expect(problems.has_value());
        if (problems) {
            for (const auto& p : *problems) std::println(std::cerr, "  unexpected problem: {}", p);
            expect(problems->empty());
        }
        auto product = version::manifest_version(root);
        expect(product.has_value() && *product == "2026.9.16.1");
        auto extension = version::extension_version(*product);
        expect(extension.has_value() && *extension == "2026.916.1");
        std::filesystem::remove_all(root);
    };

    "set_everywhere writes the product version and its mapped extension version to every site"_test = [&] {
        const std::string root { make_consistent_root() };
        auto wanted = version::set_everywhere(root, "2026.10.5.2");
        expect(wanted.has_value());
        if (wanted) expect(*wanted == "2026.1005.2");

        expect(version::manifest_version(root).value_or("") == "2026.10.5.2");
        expect(version::module_version(root).value_or("") == "2026.10.5.2");
        expect(version::extension_manifest_version(root).value_or("") == "2026.1005.2");
        expect(version::zed_manifest_version(root).value_or("") == "2026.1005.2");
        expect(version::clion_plugin_version(root).value_or("") == "2026.10.5.2");

        auto problems = version::check(root);
        expect(problems.has_value());
        if (problems) expect(problems->empty());
        std::filesystem::remove_all(root);
    };

    "a site that disagrees is reported by name"_test = [&] {
        const std::string root { make_consistent_root() };
        expect(version::extension_manifest_version(root, "9.999.9").has_value());
        auto problems = version::check(root);
        expect(problems.has_value());
        if (!problems) return;
        expect(!problems->empty());
        const bool namesIt = std::ranges::any_of(*problems, [](const std::string& p) { return p.contains("package.json"); });
        expect(namesIt);
        std::filesystem::remove_all(root);
    };

    "MINIMUM_MCPP_VERSION newer than the CI-tested mcpp is reported"_test = [&] {
        const std::string root { make_consistent_root() };
        write(base::join_path(root, "modules/base/src/version.cppm"),
              "export module mcppls.base.version;\n"
              "inline constexpr std::string_view VERSION { \"2026.9.16.1\" };\n"
              "inline constexpr std::string_view CLANGD_VERSION { \"23.1.0\" };\n"
              "inline constexpr std::string_view MINIMUM_MCPP_VERSION { \"2027.1.1.1\" };\n");
        auto problems = version::check(root);
        expect(problems.has_value());
        if (!problems) return;
        expect(!problems->empty());
        const bool namesIt = std::ranges::any_of(*problems, [](const std::string& p) { return p.contains("older than the MINIMUM_MCPP_VERSION"); });
        expect(namesIt);
        std::filesystem::remove_all(root);
    };

    return report();
}
