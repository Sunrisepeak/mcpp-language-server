// Visual Studio and the Windows SDK as facts: what a Clang-based engine needs to
// analyze MSVC STL code on a machine whose environment was not set up by a
// developer command prompt (usable plan W1.1).
export module mcppls.toolchain.visualstudio;

import std;

export namespace mcppls::toolchain::visualstudio {

struct Installation {
    std::string vsRoot;           // <...>/Microsoft Visual Studio/2022/Enterprise; empty when only a tools directory is known
    std::string toolsVersion;     // 14.44.35207
    std::string toolsDirectory;   // <vsRoot>/VC/Tools/MSVC/14.44.35207
    bool hasStdModules { false }; // <toolsDirectory>/modules/std.ixx exists
    std::string sdkRoot;          // C:/Program Files (x86)/Windows Kits/10
    std::string sdkVersion;       // 10.0.26100.0
};

struct DiscoveryInputs {
    // Environment lookup; VCToolsInstallDir, WindowsSdkDir and WindowsSDKVersion are declared answers.
    std::function<std::optional<std::string>(std::string_view name)> environment;
    // Runs a program and returns its standard output, or nullopt when it cannot run.
    std::function<std::optional<std::string>(std::span<const std::string> argv)> run;
    std::string vswhere { "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" };
    std::string defaultSdkRoot { "C:/Program Files (x86)/Windows Kits/10" };
    std::string recordedDriver;   // cl.exe or clang-cl as a build recorded it, if any
};

// <tools>/bin/Host<arch>/<arch>/cl.exe -> <tools>; empty for any other shape.
std::string tools_directory_of_cl(std::string_view clPath);
// <vsRoot>/VC/Tools/Llvm[/x64]/bin/clang-cl.exe -> <vsRoot>; empty for any other shape.
std::string vs_root_of_bundled_clang(std::string_view driverPath);
// The newest toolset under <vsRoot>/VC/Tools/MSVC, preferring toolsets that ship modules/std.ixx.
std::optional<Installation> installation_at(std::string_view vsRoot);
// A toolset directory taken as it is.
std::optional<Installation> installation_of_tools(std::string_view toolsDirectory);
// The newest complete SDK: Include/<v>/ucrt/corecrt.h and Lib/<v>/um/x64/kernel32.lib both present.
std::optional<std::string> newest_sdk_version(std::string_view sdkRoot);
// Installation paths from `vswhere -format json`, in the order vswhere reported them.
std::vector<std::string> parse_vswhere(std::string_view json);
// Toolset 14.44.35207 -> 19.44, the cl version series the toolset belongs to; empty when unparsable.
std::string compatibility_version(std::string_view toolsVersion);

// Environment first, then the recorded driver, then vswhere; the SDK from the
// environment, else the newest complete one under the default root.
std::optional<Installation> discover(const DiscoveryInputs& inputs);

} // namespace mcppls::toolchain::visualstudio
