// Visual Studio and Windows SDK facts from installation trees laid out in a temporary directory.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.toolchain.visualstudio;

using namespace mcppls;
namespace vs = mcppls::toolchain::visualstudio;

namespace {

std::string scratch(std::string_view name) {
    const std::string root { base::join_path(platform::dirs::temp_directory(),
        std::format("mcppls-test-vs-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)platform::fs::create_directories(root);
    return root;
}

void touch(const std::string& path) {
    (void)platform::fs::create_directories(base::parent_path(path));
    (void)platform::fs::write_file(path, "");
}

// <vs>/VC/Tools/MSVC/<version>/include, with modules/std.ixx when asked.
std::string toolset(const std::string& vsRoot, std::string_view version, bool modules) {
    const std::string tools { base::join_path(vsRoot, std::format("VC/Tools/MSVC/{}", version)) };
    touch(base::join_path(tools, "include/yvals_core.h"));
    touch(base::join_path(tools, "bin/Hostx64/x64/cl.exe"));
    if (modules) {
        touch(base::join_path(tools, "modules/std.ixx"));
        touch(base::join_path(tools, "modules/modules.json"));
    }
    return tools;
}

void sdk(const std::string& root, std::string_view version, bool complete) {
    touch(base::join_path(root, std::format("Include/{}/ucrt/corecrt.h", version)));
    if (complete) touch(base::join_path(root, std::format("Lib/{}/um/x64/kernel32.lib", version)));
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "paths a build records"_test = [] {
        expect(vs::tools_directory_of_cl("C:/VS/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe") == "C:/VS/VC/Tools/MSVC/14.44.35207")
            << vs::tools_directory_of_cl("C:/VS/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe");
        expect(vs::tools_directory_of_cl("C:/LLVM/bin/clang-cl.exe").empty());
        expect(vs::tools_directory_of_cl("C:/tools/cl.exe").empty());
        expect(vs::vs_root_of_bundled_clang("C:/VS/2022/Enterprise/VC/Tools/Llvm/x64/bin/clang-cl.exe") == "C:/VS/2022/Enterprise");
        expect(vs::vs_root_of_bundled_clang("C:/VS/2022/Enterprise/VC/Tools/Llvm/bin/clang++.exe") == "C:/VS/2022/Enterprise");
        expect(vs::vs_root_of_bundled_clang("C:/Program Files/LLVM/bin/clang-cl.exe").empty());
        expect(vs::compatibility_version("14.44.35207") == "19.44");
        expect(vs::compatibility_version("15.0").empty() && vs::compatibility_version("x").empty());
    };

    "vswhere output"_test = [] {
        const auto paths = vs::parse_vswhere(R"([{"installationPath": "C:\\VS\\2022\\Enterprise", "catalog": {}}, {"instanceId": "x"}, 3])");
        expect(paths.size() == 1u && paths.front().ends_with("Enterprise")) << std::format("{}", paths);
        expect(vs::parse_vswhere("not json").empty());
    };

    "the newest toolset with the std module wins"_test = [] {
        const std::string root { scratch("toolsets") };
        (void)toolset(root, "14.29.30133", false);
        const std::string modern { toolset(root, "14.44.35207", true) };
        (void)toolset(root, "14.50.1", false);   // newer, but without modules
        const auto installation = vs::installation_at(root);
        expect(fatal(installation.has_value()));
        expect(installation->toolsVersion == "14.44.35207") << installation->toolsVersion;
        expect(installation->hasStdModules);
        expect(base::same_path(installation->toolsDirectory, modern));
        expect(base::same_path(installation->vsRoot, root));
        platform::fs::remove_all(root);
    };

    "only a complete SDK counts"_test = [] {
        const std::string root { scratch("sdk") };
        sdk(root, "10.0.22621.0", true);
        sdk(root, "10.0.26100.0", false);   // headers without import libraries
        const auto version = vs::newest_sdk_version(root);
        expect(version == std::optional<std::string> { "10.0.22621.0" }) << version.value_or("none");
        platform::fs::remove_all(root);
    };

    "discovery prefers what the environment declares"_test = [] {
        const std::string root { scratch("discover") };
        const std::string vsRoot { base::join_path(root, "VS/2022/Enterprise") };
        const std::string older { toolset(vsRoot, "14.43.1", true) };
        const std::string newer { toolset(vsRoot, "14.44.35207", true) };
        const std::string kits { base::join_path(root, "Windows Kits/10") };
        sdk(kits, "10.0.22621.0", true);
        sdk(kits, "10.0.26100.0", true);
        const std::string vswhere { base::join_path(root, "vswhere.exe") };
        touch(vswhere);
        std::map<std::string, std::string> environment;
        int vswhereRuns { 0 };
        vs::DiscoveryInputs inputs;
        inputs.environment = [&](std::string_view name) -> std::optional<std::string> {
            const auto it = environment.find(std::string { name });
            return it == environment.end() ? std::nullopt : std::optional<std::string> { it->second };
        };
        inputs.run = [&](std::span<const std::string> argv) -> std::optional<std::string> {
            ++vswhereRuns;
            expect(argv.size() > 1 && argv[0] == vswhere);
            return std::format(R"([{{"installationPath": "{}"}}])", vsRoot);
        };
        inputs.vswhere = vswhere;
        inputs.defaultSdkRoot = kits;

        // vswhere: the newest toolset, the newest SDK.
        auto found = vs::discover(inputs);
        expect(fatal(found.has_value()));
        expect(found->toolsVersion == "14.44.35207" && found->sdkVersion == "10.0.26100.0") << found->toolsVersion << " " << found->sdkVersion;
        expect(base::same_path(found->sdkRoot, kits));
        expect(vswhereRuns == 1);

        // A recorded cl.exe names its own toolset, without vswhere.
        inputs.recordedDriver = base::join_path(older, "bin/Hostx64/x64/cl.exe");
        found = vs::discover(inputs);
        expect(fatal(found.has_value()));
        expect(found->toolsVersion == "14.43.1") << found->toolsVersion;
        expect(vswhereRuns == 1);

        // A developer environment outranks both.
        environment["VCToolsInstallDir"] = newer + "/";
        environment["WindowsSdkDir"] = kits + "\\";
        environment["WindowsSDKVersion"] = "10.0.22621.0\\";
        found = vs::discover(inputs);
        expect(fatal(found.has_value()));
        expect(found->toolsVersion == "14.44.35207" && found->sdkVersion == "10.0.22621.0") << found->toolsVersion << " " << found->sdkVersion;
        expect(vswhereRuns == 1);

        // Nothing installed.
        environment.clear();
        inputs.recordedDriver.clear();
        inputs.run = [](std::span<const std::string>) -> std::optional<std::string> { return "[]"; };
        expect(!vs::discover(inputs).has_value());
        platform::fs::remove_all(root);
    };

    return report();
}
