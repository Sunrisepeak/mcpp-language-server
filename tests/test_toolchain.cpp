// Compile databases, driver classification and probing with recorded answers.
import std;
import mcppls.testing;
import nlohmann.json;
import mcppls.base.error;
import mcppls.platform.process;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.base.path;
import mcppls.spec.database;
import mcppls.toolchain.probe;
import mcppls.project.compdb;

using namespace mcppls;
using Json = nlohmann::json;

namespace {

// Answers queries from a table keyed by the joined argv.
toolchain::Runner recorded(std::map<std::string, std::string> answers, std::map<std::string, std::string> errors = {}) {
    return [answers = std::move(answers), errors = std::move(errors)](std::span<const std::string> argv) -> base::Result<platform::RunResult> {
        std::string key;
        for (const auto& argument : argv) key += (key.empty() ? "" : " ") + argument;
        platform::RunResult result;
        if (auto it = answers.find(key); it != answers.end()) {
            result.exitCode = 0;
            result.output = it->second;
        } else if (auto error = errors.find(key); error != errors.end()) {
            result.exitCode = 0;
            result.error = error->second;
        } else {
            result.exitCode = 1;
            result.error = "unrecorded query: " + key;
        }
        return result;
    };
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "POSIX command splitting"_test = [] {
        using project::CommandSyntax;
        const auto words = project::split_command(R"(/usr/bin/g++ -DNAME="a b" '-DQ=it'"'"'s' -I/x\ y -c "src/m.cppm")", CommandSyntax::posix);
        const std::vector<std::string> expected { "/usr/bin/g++", "-DNAME=a b", "-DQ=it's", "-I/x y", "-c", "src/m.cppm" };
        expect(words == expected) << std::format("{}", words);
    };

    "Windows command splitting"_test = [] {
        using project::CommandSyntax;
        // CommandLineToArgvW: backslashes are literal unless they precede a quote.
        const auto words = project::split_command(R"(C:\LLVM\bin\clang-cl.exe /DNAME="a b" "C:\Program Files\x" /I"C:\a b\\" x\\y "q\"uote" "")", CommandSyntax::windows);
        const std::vector<std::string> expected { R"(C:\LLVM\bin\clang-cl.exe)", "/DNAME=a b", R"(C:\Program Files\x)", R"(/IC:\a b\)", R"(x\\y)", R"(q"uote)", "" };
        expect(words == expected) << std::format("{}", words);
    };

    "compile_commands entries in both forms"_test = [] {
        const auto document = Json::parse(R"([
          {"directory": "/p/build", "file": "../src/a.cppm", "arguments": ["clang++", "-c", "../src/a.cppm"], "output": "a.o"},
          {"directory": "/p/build", "file": "/p/src/main.cpp", "command": "g++ -std=c++23 -c /p/src/main.cpp"},
          {"directory": "/p", "file": "x.cpp"},
          "garbage"
        ])");
        auto commands = project::parse_compile_commands(document, project::CommandSyntax::posix);
        expect(fatal(commands.has_value()));
        expect(fatal(commands->size() == 2u));
        expect((*commands)[0].file == "/p/src/a.cppm");
        expect((*commands)[0].output == "a.o");
        expect((*commands)[1].arguments.size() == 4u);
        expect(!project::parse_compile_commands(Json::object(), project::CommandSyntax::posix).has_value());
    };

    "driver families"_test = [] {
        expect(toolchain::classify_driver("/usr/bin/g++") == mcppls::spec::Family::gcc);
        expect(toolchain::classify_driver("/opt/gcc/bin/x86_64-w64-mingw32-g++") == mcppls::spec::Family::gcc);
        expect(toolchain::classify_driver("/usr/bin/g++-15") == mcppls::spec::Family::gcc);
        expect(toolchain::classify_driver("/usr/bin/c++") == mcppls::spec::Family::gcc);
        expect(toolchain::classify_driver("/llvm/bin/clang++") == mcppls::spec::Family::clang);
        expect(toolchain::classify_driver("/llvm/bin/x86_64-w64-mingw32-clang++") == mcppls::spec::Family::clang);
        expect(toolchain::classify_driver("C:/LLVM/bin/clang-cl.exe") == mcppls::spec::Family::clang_cl);
        expect(toolchain::classify_driver("C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe") == mcppls::spec::Family::msvc);
        expect(toolchain::classify_driver("/usr/bin/ccache") == mcppls::spec::Family::other);
        expect(toolchain::classify_driver("/usr/bin/nvcc") == mcppls::spec::Family::other);
    };

    "relevant arguments"_test = [] {
        const std::vector<std::string> arguments { "clang++", "-O2", "--target=x86_64-w64-mingw32", "-stdlib=libc++", "-target", "t",
                                                   "--sysroot", "/s", "-DX", "-c", "a.cpp" };
        const std::vector<std::string> expected { "--target=x86_64-w64-mingw32", "-stdlib=libc++", "-target", "t", "--sysroot", "/s" };
        expect(toolchain::probe_relevant_arguments(arguments) == expected);
    };

    "GCC 16 on Linux"_test = [] {
        const std::string g { "/opt/gcc/16.1.0/bin/g++" };
        const auto runner = recorded({
            { g + " -dumpmachine", "x86_64-linux-gnu\n" },
            { g + " -dumpfullversion", "16.1.0\n" },
            { g + " -print-file-name=libstdc++.modules.json", "/opt/gcc/16.1.0/bin/../lib/gcc/x86_64-linux-gnu/16.1.0/../../../../lib64/libstdc++.modules.json\n" },
            { g + " -print-libgcc-file-name", "/opt/gcc/16.1.0/bin/../lib/gcc/x86_64-linux-gnu/16.1.0/libgcc.a\n" },
        });
        auto facts = toolchain::probe_toolchain(g, std::vector<std::string> {}, runner);
        expect(fatal(facts.has_value())) << (facts ? "" : facts.error().message);
        expect(facts->toolchain.family == mcppls::spec::Family::gcc);
        expect(facts->toolchain.version == "16.1.0");
        expect(facts->toolchain.target == "x86_64-linux-gnu");
        expect(fatal(facts->toolchain.stdlib.has_value()));
        expect(facts->toolchain.stdlib->name == "libstdc++");
        expect(facts->toolchain.stdlib->moduleMetadata == "/opt/gcc/16.1.0/lib64/libstdc++.modules.json") << facts->toolchain.stdlib->moduleMetadata;
        expect(facts->gccInstallDirectory == "/opt/gcc/16.1.0/lib/gcc/x86_64-linux-gnu/16.1.0") << facts->gccInstallDirectory;
        expect(facts->mingwRoot.empty());
        expect(toolchain::toolchain_id(facts->toolchain) == "gcc-16.1.0-x86_64-linux-gnu");
    };

    "GCC 16 for MinGW"_test = [] {
        const std::string g { "/opt/mingw/16.1.0/bin/x86_64-w64-mingw32-g++" };
        const auto runner = recorded({
            { g + " -dumpmachine", "x86_64-w64-mingw32" },
            { g + " -dumpfullversion", "16.1.0" },
            { g + " -print-file-name=libstdc++.modules.json", "libstdc++.modules.json" },
        });
        auto facts = toolchain::probe_toolchain(g, std::vector<std::string> {}, runner);
        expect(fatal(facts.has_value()));
        expect(facts->mingwRoot == "/opt/mingw/16.1.0");
        expect(facts->toolchain.stdlib.has_value() && facts->toolchain.stdlib->moduleMetadata.empty());
    };

    "Clang 22 with libc++ and a configuration file"_test = [] {
        const std::string c { "/opt/llvm/22.1.8/bin/clang++" };
        const auto runner = recorded({
            { c + " --version", "clang version 22.1.8 (https://github.com/llvm/llvm-project ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)\nTarget: x86_64-unknown-linux-gnu\nThread model: posix\nInstalledDir: /opt/llvm/22.1.8/bin\nConfiguration file: /opt/llvm/22.1.8/bin/clang++.cfg\n" },
            { c + " -print-target-triple", "x86_64-unknown-linux-gnu" },
            { c + " -print-resource-dir", "/opt/llvm/22.1.8/lib/clang/22" },
            { c + " -print-library-module-manifest-path", "/opt/llvm/22.1.8/bin/../lib/x86_64-unknown-linux-gnu/libc++.modules.json" },
        });
        auto facts = toolchain::probe_toolchain(c, std::vector<std::string> {}, runner);
        expect(fatal(facts.has_value()));
        expect(facts->toolchain.version == "22.1.8");
        expect(facts->toolchain.buildId == "ca7933e47d3a3451d81e72ac174dcb5aa28b59d1");
        expect(facts->toolchain.configFiles == std::vector<std::string> { "/opt/llvm/22.1.8/bin/clang++.cfg" });
        expect(fatal(facts->toolchain.stdlib.has_value()));
        expect(facts->toolchain.stdlib->name == "libc++" && facts->toolchain.stdlib->version == "22.1.8");
        expect(facts->toolchain.stdlib->moduleMetadata == "/opt/llvm/22.1.8/lib/x86_64-unknown-linux-gnu/libc++.modules.json");
        expect(!facts->appleClang);
    };

    "Clang with libc++ chosen by include paths"_test = [] {
        const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-probe-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        const std::string manifest { mcppls::base::join_path(root, "llvm/lib/x86_64-unknown-linux-gnu/libc++.modules.json") };
        (void)mcppls::platform::fs::create_directories(mcppls::base::parent_path(manifest));
        (void)mcppls::platform::fs::write_file(manifest, "{}");
        const std::string c { mcppls::base::join_path(root, "llvm/bin/clang++") };
        const std::vector<std::string> command { c, "-std=c++23", "--no-default-config", "-nostdinc++",
                                                 "-isystem" + mcppls::base::join_path(root, "llvm/include/c++/v1"), "-c", "a.cpp" };
        const auto relevant = toolchain::probe_relevant_arguments(command);
        expect(relevant.size() == 3u) << std::format("{}", relevant);
        std::string prefix { c };
        for (const auto& argument : relevant) prefix += " " + argument;
        const auto runner = recorded({
            { prefix + " --version", "clang version 22.1.8 (https://github.com/llvm/llvm-project ca7933e47d3a)\n" },
            { prefix + " -print-target-triple", "x86_64-unknown-linux-gnu" },
            { prefix + " -print-library-module-manifest-path", "<NOT PRESENT>" },
        });
        auto facts = toolchain::probe_toolchain(c, relevant, runner);
        expect(fatal(facts.has_value()));
        expect(fatal(facts->toolchain.stdlib.has_value()));
        expect(facts->toolchain.stdlib->name == "libc++");
        expect(facts->toolchain.stdlib->moduleMetadata == manifest) << facts->toolchain.stdlib->moduleMetadata;
        mcppls::platform::fs::remove_all(root);
    };

    "Clang building against libstdc++"_test = [] {
        const std::string c { "/usr/bin/clang++" };
        const std::vector<std::string> relevant { "-stdlib=libstdc++" };
        const auto runner = recorded({
            { c + " -stdlib=libstdc++ --version", "Ubuntu clang version 18.1.3 (1ubuntu1)\nTarget: x86_64-pc-linux-gnu\n" },
            { c + " -stdlib=libstdc++ -print-target-triple", "x86_64-pc-linux-gnu" },
            { c + " -stdlib=libstdc++ -print-library-module-manifest-path", "<NOT PRESENT>" },
            { c + " -stdlib=libstdc++ -print-file-name=libstdc++.modules.json", "/usr/lib/gcc/x86_64-linux-gnu/15/libstdc++.modules.json" },
        });
        auto facts = toolchain::probe_toolchain(c, relevant, runner);
        expect(fatal(facts.has_value()));
        expect(facts->toolchain.version == "18.1.3");
        expect(facts->toolchain.stdlib.has_value() && facts->toolchain.stdlib->name == "libstdc++");
    };

    "Apple clang is recognized"_test = [] {
        const std::string c { "/usr/bin/clang++" };
        const auto runner = recorded({ { c + " --version", "Apple clang version 17.0.0 (clang-1700.0.13.5)\nTarget: arm64-apple-darwin24.5.0\n" },
                                       { c + " -print-target-triple", "arm64-apple-darwin24.5.0" } });
        auto facts = toolchain::probe_toolchain(c, std::vector<std::string> {}, runner);
        expect(fatal(facts.has_value()));
        expect(facts->appleClang);
    };

    "cl.exe banner"_test = [] {
        const std::string cl { "C:/VS/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe" };
        const auto runner = recorded({}, { { cl, "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211 for x64\nCopyright (C) Microsoft Corporation.  All rights reserved.\n" } });
        auto facts = toolchain::probe_toolchain(cl, std::vector<std::string> {}, runner);
        expect(fatal(facts.has_value()));
        expect(facts->toolchain.family == mcppls::spec::Family::msvc);
        expect(facts->toolchain.version == "19.44.35211");
        expect(facts->toolchain.target == "x86_64-pc-windows-msvc");
    };

    "an unanswering driver is an error"_test = [] {
        auto facts = toolchain::probe_toolchain("/nowhere/g++", std::vector<std::string> {}, recorded({}));
        expect(!facts.has_value());
        expect(!toolchain::probe_toolchain("/usr/bin/ccache", std::vector<std::string> {}, recorded({})).has_value());
    };

    "facts survive the cache format"_test = [] {
        toolchain::ToolchainFacts facts;
        facts.toolchain.family = mcppls::spec::Family::gcc;
        facts.toolchain.version = "16.1.0";
        facts.toolchain.driver = "/g++";
        facts.toolchain.target = "x86_64-linux-gnu";
        facts.toolchain.stdlib = mcppls::spec::Stdlib { "libstdc++", "16.1.0", "/m.json" };
        facts.gccInstallDirectory = "/lib/gcc";
        const auto restored = toolchain::facts_from_json(toolchain::facts_to_json(facts));
        expect(fatal(restored.has_value()));
        expect(restored->gccInstallDirectory == "/lib/gcc");
        expect(restored->toolchain.stdlib->moduleMetadata == "/m.json");
    };

    return report();
}
