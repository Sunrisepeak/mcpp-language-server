// Dialect translation rules (design section 14.4) and engine database plans.
import std;
import mcppls.testing;
import nlohmann.json;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.spec.database;
import mcppls.spec.kit;
import mcppls.spec.metadata;
import mcppls.toolchain.probe;
import mcppls.project.scan;
import mcppls.project.infer;
import mcppls.normalize.gnu;
import mcppls.normalize.msvc;
import mcppls.normalize.plan;
import mcppls.normalize.semantic;

namespace s = mcppls::spec;
namespace n = mcppls::normalize;
namespace p = mcppls::project;
using mcppls::toolchain::ToolchainFacts;

namespace {

bool contains(const std::vector<std::string>& words, std::string_view word) {
    return std::ranges::find(words, word) != words.end();
}

bool contains_prefix(const std::vector<std::string>& words, std::string_view prefix) {
    return std::ranges::any_of(words, [&](const std::string& word) { return word.starts_with(prefix); });
}

ToolchainFacts gcc_facts(std::string target = "x86_64-linux-gnu") {
    ToolchainFacts facts;
    facts.toolchain.family = s::Family::gcc;
    facts.toolchain.version = "16.1.0";
    facts.toolchain.driver = "/opt/gcc/bin/g++";
    facts.toolchain.target = target;
    facts.toolchain.stdlib = s::Stdlib { "libstdc++", "16.1.0", "/opt/gcc/lib64/libstdc++.modules.json" };
    if (target.find("mingw") != std::string::npos) facts.mingwRoot = "/opt/mingw";
    else facts.gccInstallDirectory = "/opt/gcc/lib/gcc/x86_64-linux-gnu/16.1.0";
    return facts;
}

ToolchainFacts msvc_facts(s::Family family) {
    ToolchainFacts facts;
    facts.toolchain.family = family;
    facts.toolchain.version = family == s::Family::msvc ? "19.44.35228" : "20.1.8";
    facts.toolchain.driver = "C:/VS/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe";
    facts.toolchain.target = "x86_64-pc-windows-msvc";
    facts.toolchain.stdlib = s::Stdlib { "msvc-stl", "14.44.35207", "C:/VS/VC/Tools/MSVC/14.44.35207/modules/modules.json" };
    facts.msvc = mcppls::toolchain::MsvcEnvironment { "C:/VS/VC/Tools/MSVC/14.44.35207", "14.44.35207", "C:/Windows Kits/10", "10.0.26100.0" };
    facts.msCompatibilityVersion = family == s::Family::msvc ? "19.44.35228" : "19.44";
    return facts;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "P1: GCC on Linux"_test = [] {
        const auto facts = gcc_facts();
        const std::vector<std::string> arguments { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-fmodule-mapper=/p/m.map", "-O0", "-g",
            "--sysroot=/opt/subos", "-B/opt/binutils/bin", "-fdeps-format=p1689r5", "-MD", "-MF", "x.d", "-c", "src/greet/greet.cppm", "-o", "obj/greet.o" };
        const auto out = n::translate_gnu(n::GnuInput { arguments, "/p/src/greet/greet.cppm", "/p", &facts, true });
        expect(contains(out, "-std=c++23") && contains(out, "-O0") && contains(out, "--sysroot=/opt/subos"));
        expect(!contains(out, "-fmodules") && !contains_prefix(out, "-fmodule-mapper") && !contains_prefix(out, "-B") && !contains_prefix(out, "-fdeps"));
        expect(!contains(out, "-c") && !contains(out, "-o") && !contains(out, "obj/greet.o") && !contains(out, "src/greet/greet.cppm"));
        expect(!contains(out, "-MD") && !contains(out, "-MF") && !contains(out, "x.d"));
        expect(contains(out, "--no-default-config") && contains(out, "--target=x86_64-linux-gnu") && contains(out, "-stdlib=libstdc++"));
        expect(contains(out, "--gcc-install-dir=/opt/gcc/lib/gcc/x86_64-linux-gnu/16.1.0"));
        expect(out.size() >= 2 && out[out.size() - 2] == "-x" && out.back() == "c++-module");
    };

    "P2: GCC for MinGW uses the sysroot"_test = [] {
        const auto facts = gcc_facts("x86_64-w64-mingw32");
        const std::vector<std::string> arguments { "/opt/mingw/bin/x86_64-w64-mingw32-g++", "-std=c++23", "-c", "/p/main.cpp" };
        const auto out = n::translate_gnu(n::GnuInput { arguments, "/p/main.cpp", "/p", &facts, false });
        expect(contains(out, "--sysroot=/opt/mingw"));
        expect(!contains_prefix(out, "--gcc-install-dir"));
        expect(!contains(out, "c++-module"));
    };

    "P3: Clang strips BMI arguments and keeps the rest"_test = [] {
        ToolchainFacts facts;
        facts.toolchain.family = s::Family::clang;
        facts.toolchain.driver = "/llvm/bin/clang++";
        const std::vector<std::string> arguments { "/llvm/bin/clang++", "-std=c++23", "-stdlib=libc++", "-fmodule-file=a=/b/a.pcm",
            "-fmodule-output=/b/m.pcm", "-fprebuilt-module-path=/b", "-fmodules-reduced-bmi", "@CMakeFiles/m.dir/m.cppm.o.modmap",
            "-x", "c++-module", "--precompile", "-DX=1", "-I/p/include", "-c", "/p/m.cppm", "-o", "m.o" };
        const auto out = n::translate_gnu(n::GnuInput { arguments, "/p/m.cppm", "/p/build", &facts, true });
        expect(contains(out, "-stdlib=libc++") && contains(out, "-DX=1") && contains(out, "-I/p/include"));
        expect(!contains_prefix(out, "-fmodule-file") && !contains_prefix(out, "-fmodule-output") && !contains_prefix(out, "-fprebuilt"));
        expect(!contains(out, "-fmodules-reduced-bmi") && !contains(out, "--precompile") && !contains_prefix(out, "@"));
        expect(std::ranges::count(out, std::string { "c++-module" }) == 1);
        expect(!contains(out, "--no-default-config"));
    };

    "P7: a CMake cl.exe command becomes a clang++ command"_test = [] {
        const auto facts = msvc_facts(s::Family::msvc);
        // CMake's module map expanded: -interface, -ifcOutput and -reference with the dash spelling.
        const std::vector<std::string> arguments { "C:\\PROGRA~1\\MICROS~2\\2022\\ENTERP~1\\VC\\Tools\\MSVC\\1444~1.352\\bin\\Hostx64\\x64\\cl.exe",
            "/nologo", "/TP", "/DWIN32", "/D_WINDOWS", "/EHsc", "/Ob0", "/Od", "/RTC1", "-std:c++latest", "-MDd", "-Zi", "/showIncludes",
            "-interface", "-ifcOutput", "CMakeFiles\\greet.dir\\greet.ifc", "-reference", "std=CMakeFiles\\__cmake_cxx23.dir\\std.ifc",
            "/FoCMakeFiles\\greet.dir\\src\\greet.ixx.obj", "/FdCMakeFiles\\greet.dir\\greet.pdb", "/FS", "-c", "C:\\p\\src\\greet.ixx" };
        const auto out = n::translate_msvc(n::MsvcInput { arguments, "C:/p/src/greet.ixx", "C:/p/build", &facts, true });
        expect(contains(out, "-DWIN32") && contains(out, "-D_WINDOWS") && contains(out, "-std=c++26")) << std::format("{}", out);
        expect(contains(out, "-fcxx-exceptions") && contains(out, "-fexceptions") && contains(out, "-fms-runtime-lib=dll_dbg"));
        expect(!contains_prefix(out, "/") && !contains_prefix(out, "-interface") && !contains_prefix(out, "-ifc") && !contains_prefix(out, "-reference"))
            << std::format("{}", out);
        expect(!contains(out, "std=CMakeFiles\\__cmake_cxx23.dir\\std.ifc") && !contains(out, "CMakeFiles\\greet.dir\\greet.ifc"));
        expect(!contains(out, "C:\\p\\src\\greet.ixx") && !contains(out, "-c") && !contains_prefix(out, "-Zi"));
        expect(contains(out, "--no-default-config") && contains(out, "--target=x86_64-pc-windows-msvc"));
        expect(contains(out, "-fms-compatibility-version=19.44.35228"));
        expect(contains(out, "-Xmicrosoft-visualc-tools-root") && contains(out, "C:/VS/VC/Tools/MSVC/14.44.35207"));
        expect(contains(out, "-Xmicrosoft-windows-sdk-root") && contains(out, "-Xmicrosoft-windows-sdk-version") && contains(out, "10.0.26100.0"));
        expect(contains(out, "-fno-aligned-allocation"));
        expect(out.size() >= 2 && out[out.size() - 2] == "-x" && out.back() == "c++-module");
    };

    "P7: an mcpp cl.exe command"_test = [] {
        const auto facts = msvc_facts(s::Family::msvc);
        const std::vector<std::string> arguments { "C:\\VS\\VC\\Tools\\MSVC\\14.44.35207\\bin\\Hostx64\\x64\\cl.exe", "/std:c++latest", "/nologo",
            "/EHsc", "/utf-8", "/MD", "/reference", "std=D:\\w\\target\\ifc.cache\\std.ifc", "/ifcSearchDir", "D:\\w\\target\\ifc.cache",
            "/Od", "/Zi", "/FS", "-c", "D:\\w\\src\\main.cpp", "-o", "D:\\w\\target\\obj\\main.obj" };
        const auto out = n::translate_msvc(n::MsvcInput { arguments, "D:/w/src/main.cpp", "D:/w", &facts, false });
        expect(contains(out, "-std=c++26") && contains(out, "-fms-runtime-lib=dll")) << std::format("{}", out);
        expect(!contains(out, "D:\\w\\target\\ifc.cache") && !contains(out, "D:\\w\\target\\obj\\main.obj") && !contains(out, "-o"));
        expect(!contains(out, "c++-module"));
    };

    "P6: clang-cl passes GNU arguments through"_test = [] {
        const auto facts = msvc_facts(s::Family::clang_cl);
        const std::vector<std::string> arguments { "C:/LLVM/bin/clang-cl.exe", "/std:c++20", "/GR-", "/I", "C:/p/include", "/FIpch.h",
            "/external:I", "C:/deps", "/clang:-fmodule-output=C:/b/m.pcm", "/clang:-fmodule-file=std=C:/b/std.pcm", "-fmodule-file=a=C:/b/a.pcm",
            "-Wno-unused", "-mavx2", "/Zc:alignedNew-", "/c", "C:/p/src/m.ixx" };
        const auto out = n::translate_msvc(n::MsvcInput { arguments, "C:/p/src/m.ixx", "C:/p", &facts, true });
        expect(contains(out, "-std=c++20") && contains(out, "-fno-rtti") && contains(out, "-IC:/p/include")) << std::format("{}", out);
        expect(contains(out, "-include") && contains(out, "pch.h") && contains(out, "-isystem") && contains(out, "C:/deps"));
        expect(contains(out, "-Wno-unused") && contains(out, "-mavx2") && contains(out, "-fms-runtime-lib=static"));
        expect(!contains_prefix(out, "-fmodule-output") && !contains_prefix(out, "-fmodule-file"));
        expect(std::ranges::count(out, std::string { "-fno-aligned-allocation" }) == 1);
        expect(contains(out, "-fms-compatibility-version=19.44"));
    };

    "P5: clang++ for the MSVC ABI"_test = [] {
        auto facts = msvc_facts(s::Family::clang);
        facts.toolchain.driver = "C:/LLVM/bin/clang++.exe";
        const std::vector<std::string> arguments { "C:/LLVM/bin/clang++.exe", "-std=c++23", "-fmodule-file=std=D:/w/pcm.cache/std.pcm",
            "-fprebuilt-module-path=D:/w/pcm.cache", "-O0", "-g", "-c", "D:/w/src/greet.cppm", "-o", "D:/w/obj/greet.m.o" };
        const auto out = n::translate_gnu(n::GnuInput { arguments, "D:/w/src/greet.cppm", "D:/w", &facts, true });
        expect(contains(out, "--target=x86_64-pc-windows-msvc") && contains(out, "-Xmicrosoft-visualc-tools-root")) << std::format("{}", out);
        expect(contains(out, "-fno-aligned-allocation") && !contains_prefix(out, "-fmodule-file") && !contains_prefix(out, "-fprebuilt"));
        expect(out.back() == "c++-module");
    };

    "kit arguments and semantic subsets"_test = [] {
        s::Kit kit;
        kit.target = "x86_64-w64-mingw32";
        kit.arguments = { "-nostdinc++", "-nostdlibinc" };
        kit.systemIncludeDirectories = { "/kit/include/c++/v1", "/kit/include" };
        const auto out = n::kit_arguments(kit, "c++26", "");
        const std::vector<std::string> expected { "--no-default-config", "--target=x86_64-w64-mingw32", "-std=c++26", "-nostdinc++",
                                                  "-nostdlibinc", "-isystem", "/kit/include/c++/v1", "-isystem", "/kit/include" };
        expect(out == expected) << std::format("{}", out);
        const std::vector<std::string> command { "cl.exe", "/IC:/inc", "/DA=1", "/std:c++20", "-O2", "/FIpch.h", "-I", "x" };
        const auto subset = n::semantic_subset(command);
        const std::vector<std::string> wanted { "-IC:/inc", "-DA=1", "-std=c++20", "-includepch.h", "-I", "x" };
        expect(subset == wanted) << std::format("{}", subset);
        expect(n::language_standard_of(command) == "c++20");
    };

    "a plan resolves, injects std once and leaves out what cannot resolve"_test = [] {
        const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-plan-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        (void)mcppls::platform::fs::create_directories(root);
        const std::map<std::string, std::string> sources {
            { "/p/src/main.cpp", "import std;\nimport hello.greet;\nimport missing.module;\nint main() {}\n" },
            { "/p/src/app.cpp", "import std;\nimport hello.greet;\nint run() { return 0; }\n" },
            { "/p/src/greet.cppm", "export module hello.greet;\nexport import :detail;\nimport std;\n" },
            { "/p/src/detail.cppm", "export module hello.greet:detail;\nimport std;\n" },
            { "/p/src/broken.cppm", "export module broken;\nimport nowhere;\n" },
            { "/p/src/user.cppm", "export module user;\nimport broken;\n" },
            { "/p/src/dup1.cppm", "export module dup;\n" },
            { "/p/src/dup2.cppm", "export module dup;\n" },
        };
        s::Database database;
        database.hasIde = true;
        s::Set set;
        set.name = "hello";
        set.hasIde = true;
        set.toolchain = "gcc-16.1.0-x86_64-linux-gnu";
        for (const auto& [path, text] : sources) {
            s::TranslationUnit unit;
            unit.source = path;
            unit.workDirectory = "/p";
            unit.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-c", path };
            set.units.push_back(std::move(unit));
        }
        database.sets.push_back(set);
        std::map<std::string, ToolchainFacts, std::less<>> facts { { set.toolchain, gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        int manifestReads { 0 };
        input.metadataReader = [&](std::string_view manifest) {
            ++manifestReads;
            expect(manifest == "/opt/gcc/lib64/libstdc++.modules.json");
            return std::vector<s::ModuleEntry> { { "std", "/opt/gcc/include/c++/16/bits/std.cc", true, {}, {} },
                                                 { "std.compat", "/opt/gcc/include/c++/16/bits/std.compat.cc", true, {}, {} } };
        };
        const auto plan = n::plan_engine(input);
        std::set<std::string> files;
        for (const auto& entry : plan.entries) files.insert(entry.file);
        expect(files.contains("/p/src/main.cpp")) << "a unit that provides no module stays whatever it imports: clangd answers it (experiment S10)";
        expect(files.contains("/p/src/app.cpp")) << "a non-module unit whose imports resolve is written";
        expect(files.contains("/p/src/greet.cppm") && files.contains("/p/src/detail.cppm"));
        expect(!files.contains("/p/src/broken.cppm")) << "an interface with an unresolvable import is left out";
        expect(!files.contains("/p/src/user.cppm")) << "and so is an interface importing it";
        expect(files.contains("/p/src/dup1.cppm") && !files.contains("/p/src/dup2.cppm"));
        expect(files.contains("/opt/gcc/include/c++/16/bits/std.cc") && files.contains("/opt/gcc/include/c++/16/bits/std.compat.cc"));
        expect(plan.stdUnits == 2u);
        const auto has_issue = [&](std::string_view code, std::string_view module) {
            return std::ranges::any_of(plan.issues, [&](const n::PlanIssue& issue) { return issue.code == code && issue.module == module; });
        };
        expect(has_issue("unresolved-module", "missing.module"));
        expect(has_issue("unresolved-module", "nowhere"));
        expect(has_issue("ambiguous-module", "dup"));
        for (const auto& entry : plan.entries) {
            if (entry.file == "/p/src/greet.cppm") {
                expect(entry.arguments.front() == "/payload/clangd/bin/clang++");
                expect(entry.arguments.back() == "/p/src/greet.cppm");
                expect(contains(entry.arguments, "c++-module"));
            }
            if (entry.file.ends_with("std.cc")) {
                expect(contains(entry.arguments, "-Wno-reserved-module-identifier") && contains(entry.arguments, "--gcc-install-dir=/opt/gcc/lib/gcc/x86_64-linux-gnu/16.1.0"));
            }
        }
        const auto written = n::write_engine_database(root, plan);
        expect(written.has_value());
        const auto text = mcppls::platform::fs::read_file(mcppls::base::join_path(root, "compile_commands.json"));
        expect(fatal(text.has_value()));
        expect(nlohmann::json::parse(*text).size() == plan.entries.size());
        mcppls::platform::fs::remove_all(root);
    };

    "a C library listed first decides neither how std nor how C sources are built"_test = [] {
        // The shape of a workspace whose dependencies include C libraries (xlings: compat.mbedtls is its first set).
        const std::map<std::string, std::string> sources {
            { "/deps/mbedtls/library/aes.c", "int aes(void) { return 0; }\n" },
            { "/p/src/main.cpp", "import std;\nimport app.core;\nint main() {}\n" },
            { "/p/src/core.cppm", "export module app.core;\nimport std;\n" },
        };
        s::Database database;
        database.hasIde = true;
        s::Set library;
        library.name = "compat.mbedtls";
        library.hasIde = true;
        library.toolchain = "gcc-16.1.0-x86_64-linux-gnu";
        s::TranslationUnit c;
        c.source = "/deps/mbedtls/library/aes.c";
        c.workDirectory = "/deps/mbedtls";
        c.arguments = { "/opt/gcc/bin/gcc", "-I/deps/mbedtls/include", "-std=c11", "-O0", "-c", c.source };
        library.units.push_back(c);
        s::Set app;
        app.name = "app";
        app.hasIde = true;
        app.toolchain = library.toolchain;
        for (const std::string path : { "/p/src/core.cppm", "/p/src/main.cpp" }) {
            s::TranslationUnit unit;
            unit.source = path;
            unit.workDirectory = "/p";
            unit.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-O0", "-c", path };
            app.units.push_back(std::move(unit));
        }
        database.sets = { library, app };
        std::map<std::string, ToolchainFacts, std::less<>> facts { { library.toolchain, gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.primeDirectory = "/cache/prime";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        input.metadataReader = [](std::string_view) {
            return std::vector<s::ModuleEntry> { { "std", "/opt/gcc/include/c++/16/bits/std.cc", true, {}, {} } };
        };
        const auto plan = n::plan_engine(input);
        const auto entry_of = [&](std::string_view file) -> const n::EngineEntry* {
            const auto it = std::ranges::find_if(plan.entries, [&](const n::EngineEntry& entry) { return entry.file == file; });
            return it == plan.entries.end() ? nullptr : &*it;
        };
        const auto* stdEntry = entry_of("/opt/gcc/include/c++/16/bits/std.cc");
        expect(fatal(stdEntry != nullptr));
        expect(contains(stdEntry->arguments, "-std=c++23") && !contains(stdEntry->arguments, "-std=c11") && !contains(stdEntry->arguments, "-I/deps/mbedtls/include"))
            << "std takes the arguments of a C++ unit that imports it: " << std::format("{}", stdEntry->arguments);
        const auto* aes = entry_of("/deps/mbedtls/library/aes.c");
        expect(fatal(aes != nullptr));
        expect(aes->arguments.front() == "/payload/clangd/bin/clang" && contains(aes->arguments, "-std=c11"))
            << "a C source is read as C: " << std::format("{}", aes->arguments);
        const auto* core = entry_of("/p/src/core.cppm");
        expect(fatal(core != nullptr));
        expect(core->arguments.front() == "/payload/clangd/bin/clang++");
        const auto prime = std::ranges::find_if(plan.entries, [](const n::EngineEntry& entry) { return entry.file.ends_with("-std.cpp"); });
        expect(fatal(prime != plan.entries.end()));
        expect(contains(prime->arguments, "-std=c++23") && !contains(prime->arguments, "-std=c11")) << std::format("{}", prime->arguments);
    };

    "a module nothing provides gets a stand-in, and every unit stays"_test = [] {
        // robustness design C2: with stand-ins every import resolves, so clangd never builds a module whose import it
        // cannot resolve (experiments S2, S6); only the names the missing module would declare are missing.
        const std::map<std::string, std::string> sources {
            { "/p/src/main.cpp", "import hello.greet;\nimport missing.module;\nint main() {}\n" },
            { "/p/src/greet.cppm", "export module hello.greet;\n" },
            { "/p/src/broken.cppm", "export module broken;\nimport nowhere;\n" },
            { "/p/src/user.cppm", "export module user;\nimport broken;\n" },
            { "/p/src/lost.cppm", "export module lost;\n" },
            { "/p/src/usedlost.cpp", "import lost;\nint f() { return 0; }\n" },
        };
        s::Database database;
        database.hasIde = true;
        s::Set set;
        set.name = "hello";
        set.hasIde = true;
        set.toolchain = "gcc-16.1.0-x86_64-linux-gnu";
        for (const auto& [path, text] : sources) {
            s::TranslationUnit unit;
            unit.source = path;
            unit.workDirectory = "/p";
            unit.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-c", path };
            set.units.push_back(std::move(unit));
        }
        database.sets.push_back(set);
        std::map<std::string, ToolchainFacts, std::less<>> facts { { set.toolchain, gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.stubDirectory = "/cache/stubs";
        input.moduleHintDirectory = "/cache/hints";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        // clangd reported that it cannot find `lost` although the plan has its unit.
        input.unresolvedModules = { { "lost", "Don't get the module unit for module lost" } };
        const auto plan = n::plan_engine(input);
        std::map<std::string, const n::EngineEntry*> byFile;
        std::map<std::string, const n::EngineEntry*> byModule;
        for (const auto& entry : plan.entries) {
            byFile[entry.file] = &entry;
            if (!entry.provides.empty()) byModule[entry.provides] = &entry;
        }
        for (const std::string file : { "/p/src/main.cpp", "/p/src/greet.cppm", "/p/src/broken.cppm", "/p/src/user.cppm", "/p/src/usedlost.cpp" }) {
            expect(byFile.contains(file)) << file << " stays";
        }
        expect(!byFile.contains("/p/src/lost.cppm")) << "a unit clangd cannot find is no use to it";
        const std::vector<std::string> wanted { "lost", "missing.module", "nowhere" };
        auto stubs = plan.stubModules;
        std::ranges::sort(stubs);
        expect(stubs == wanted) << std::format("{}", stubs);
        for (const auto& name : wanted) {
            expect(fatal(byModule.contains(name)));
            const auto& entry = *byModule[name];
            expect(entry.file.starts_with("/cache/stubs/") && entry.file.ends_with(".cppm") && entry.imports.empty());
            expect(contains(entry.arguments, "c++-module") && contains(entry.arguments, "-std=c++23") && entry.arguments.back() == entry.file);
        }
        expect(std::ranges::any_of(plan.stubSources, [](const auto& source) { return source.second == "export module nowhere;\n"; }));
        expect(contains(byFile["/p/src/user.cppm"]->moduleHints, "-fmodule-file=nowhere=/cache/hints/nowhere.pcm")) << "the stand-in is named to clangd like any unit";
        expect(plan.excludedFiles == std::vector<std::string> { "/p/src/lost.cppm" }) << std::format("{}", plan.excludedFiles);
    };

    "an import still being typed gets no stand-in, and a name no module can have is never planned"_test = [] {
        // import-hang plan §5: typing `import hello.greet;` goes through `import hello` and `import hello.`; with autosave
        // each reached the plan, got a stand-in and rewrote the engine database. A unit that provides a module still gets
        // its stand-in at once: building it with an import it cannot resolve is what stalls clangd.
        const std::map<std::string, std::string> sources {
            { "/p/src/main.cpp", "import hello;\nint main() {}\n" },
            { "/p/src/greet.cppm", "export module hello.greet;\nimport half;\n" },
            { "/p/src/other.cpp", "import gone;\nint f() { return 0; }\n" },
        };
        s::Database database;
        database.hasIde = true;
        s::Set set;
        set.name = "hello";
        set.hasIde = true;
        set.toolchain = "gcc-16.1.0-x86_64-linux-gnu";
        for (const auto& [path, text] : sources) {
            s::TranslationUnit unit;
            unit.source = path;
            unit.workDirectory = "/p";
            unit.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-c", path };
            // What a build tool's scan of a file saved mid-edit reported: `hello.` is no module name.
            if (path == "/p/src/other.cpp") unit.requiredModules = { "gone", "hello.", ".x", "a..b", "a:b:c" };
            set.units.push_back(std::move(unit));
        }
        database.sets.push_back(set);
        std::map<std::string, ToolchainFacts, std::less<>> facts { { set.toolchain, gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.stubDirectory = "/cache/stubs";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        // clangd said it cannot find `hello`, as it does for an import being typed.
        input.unresolvedModules = { { "hello", "Don't get the module unit for module hello" } };
        input.editingSources = { "/p/src/main.cpp", "/p/src/greet.cppm" };
        const auto editing = n::plan_engine(input);
        auto stubs = editing.stubModules;
        std::ranges::sort(stubs);
        expect(stubs == std::vector<std::string> { "gone", "half" }) << "only the provider's and the quiet file's: " << std::format("{}", stubs);
        expect(editing.standInsDeferred);
        expect(editing.excludedFiles.empty()) << std::format("{}", editing.excludedFiles);
        for (const auto& issue : editing.issues) {
            expect(issue.module != "hello." && issue.module != ".x" && issue.module != "a..b" && issue.module != "a:b:c") << issue.module;
            if (issue.code == "unresolved-module" || issue.code == "module-build-failed") expect(issue.category == "code") << issue.code;
        }
        expect(std::ranges::any_of(editing.issues, [](const n::PlanIssue& issue) { return issue.code == "module-build-failed" && issue.module == "hello"; }))
            << "the import is still reported";

        input.editingSources.clear();
        const auto quiet = n::plan_engine(input);
        stubs = quiet.stubModules;
        std::ranges::sort(stubs);
        expect(stubs == std::vector<std::string> { "gone", "half", "hello" }) << "once quiet, every stand-in: " << std::format("{}", stubs);
        expect(!quiet.standInsDeferred);
    };

    "a file the editor opened that no set describes joins with the nearest unit's arguments"_test = [] {
        // robustness design C2: clangd guessed the command of xlings' apps/gui/main.cpp, a target of a feature the build did not
        // enable, and without a stand-in for the module it imports that nothing provides, kept a core busy for good.
        const std::map<std::string, std::string> sources {
            { "/p/src/core/a.cppm", "export module core.a;\n" },
            { "/p/src/main.cpp", "import core.a;\nint main() {}\n" },
            { "/p/apps/tui/main.cpp", "import tui.view;\nint main() {}\n" },
            { "/p/apps/tui/view.cppm", "export module tui.view;\nimport core.a;\n" },
            { "/p/apps/gui/main.cpp", "import core.a;\nimport gui.window;\nint main() {}\n" },
            { "/p/apps/gui/panel.cppm", "export module gui.panel;\nimport core.a;\n" },
        };
        s::Database database;
        database.hasIde = true;
        for (const auto& [name, prefix, define] : { std::tuple { "core", "/p/src/", "-DCORE" }, std::tuple { "tui", "/p/apps/tui/", "-DTUI" } }) {
            s::Set set;
            set.name = name;
            set.hasIde = true;
            set.toolchain = "gcc-16.1.0-x86_64-linux-gnu";
            for (const auto& [path, text] : sources) {
                if (!path.starts_with(prefix)) continue;
                s::TranslationUnit unit;
                unit.source = path;
                unit.workDirectory = "/p";
                unit.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", define, "-c", path };
                set.units.push_back(std::move(unit));
            }
            database.sets.push_back(std::move(set));
        }
        std::map<std::string, ToolchainFacts, std::less<>> facts { { "gcc-16.1.0-x86_64-linux-gnu", gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.stubDirectory = "/cache/stubs";
        input.moduleHintDirectory = "/cache/hints";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        input.openSources = { "/p/apps/gui/main.cpp", "/p/apps/gui/panel.cppm", "/p/src/main.cpp" };
        const auto plan = n::plan_engine(input);
        std::map<std::string, std::vector<const n::EngineEntry*>> byFile;
        for (const auto& entry : plan.entries) byFile[entry.file].push_back(&entry);
        expect(plan.openSources == std::vector<std::string> { "/p/apps/gui/main.cpp", "/p/apps/gui/panel.cppm" }) << std::format("{}", plan.openSources);
        expect(byFile["/p/src/main.cpp"].size() == 1u) << "a unit of a set is not planned twice";
        expect(fatal(byFile["/p/apps/gui/main.cpp"].size() == 1u && byFile["/p/apps/gui/panel.cppm"].size() == 1u));
        const auto& main = *byFile["/p/apps/gui/main.cpp"].front();
        expect(contains(main.arguments, "-DTUI") && !contains(main.arguments, "-DCORE")) << "the nearest unit's arguments: " << std::format("{}", main.arguments);
        expect(!contains(main.arguments, "c++-module") && main.arguments.back() == "/p/apps/gui/main.cpp" && main.provides.empty());
        expect(contains(main.moduleHints, "-fmodule-file=core.a=/cache/hints/core.a.pcm") && contains(main.moduleHints, "-fmodule-file=gui.window=/cache/hints/gui.window.pcm"))
            << std::format("{}", main.moduleHints);
        const auto& panel = *byFile["/p/apps/gui/panel.cppm"].front();
        expect(panel.provides == "gui.panel" && contains(panel.arguments, "c++-module") && contains(panel.arguments, "-DTUI"));
        expect(plan.stubModules == std::vector<std::string> { "gui.window" }) << "the module nothing provides gets a stand-in: " << std::format("{}", plan.stubModules);
    };

    // real-project plan RP3.4: a file opened while browsing a workspace may belong to a different project
    // nested inside it (a conformance fixture, a vendored copy, an example with its own manifest).
    // Borrowing the nearest unit's arguments for it, as the test above does for a target the same
    // project's own build did not enable, would fold that other project's modules into this one's
    // plan instead -- exactly what showed up as spurious unresolved/ambiguous-module issues against
    // mcppls's own repository (conformance fixtures and tools/bench each have their own manifest).
    "an opened file inside a nested project's own manifest is not folded into this plan"_test = [] {
        const std::string root { mcppls::base::join_path(mcppls::platform::dirs::temp_directory(),
            std::format("mcppls-test-plan-boundary-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        const std::string coreSource { mcppls::base::join_path(root, "src/core/a.cppm") };
        const std::string vendoredMain { mcppls::base::join_path(root, "vendor/fixture/src/main.cpp") };
        (void)mcppls::platform::fs::create_directories(mcppls::base::parent_path(coreSource));
        (void)mcppls::platform::fs::create_directories(mcppls::base::parent_path(vendoredMain));
        (void)mcppls::platform::fs::write_file(coreSource, "export module core.a;\n");
        (void)mcppls::platform::fs::write_file(vendoredMain, "import missing.from.the.fixture;\nint main() {}\n");
        // What marks "vendor/fixture" as its own project rather than more of this one's sources.
        (void)mcppls::platform::fs::write_file(mcppls::base::join_path(root, "vendor/fixture/mcpp.toml"), "[package]\nname = \"fixture\"\n");

        s::Database database;
        database.hasIde = true;
        s::Set set;
        set.name = "hello";
        set.hasIde = true;
        set.toolchain = "gcc-16.1.0-x86_64-linux-gnu";
        s::TranslationUnit unit;
        unit.source = coreSource;
        unit.workDirectory = root;
        unit.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-c", coreSource };
        set.units.push_back(std::move(unit));
        database.sets.push_back(std::move(set));
        std::map<std::string, ToolchainFacts, std::less<>> facts { { "gcc-16.1.0-x86_64-linux-gnu", gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = p::file_scanner();
        input.openSources = { vendoredMain };
        const auto plan = n::plan_engine(input);
        expect(plan.openSources.empty()) << "the fixture's own file is left for its own project to serve" << std::format("{}", plan.openSources);
        expect(std::ranges::none_of(plan.entries, [&](const n::EngineEntry& entry) { return entry.file == vendoredMain; }));
        expect(plan.issues.empty()) << "no spurious unresolved-module for the fixture's own deliberately-missing import: "
                                    << std::format("{}", plan.issues.size());
        mcppls::platform::fs::remove_all(root);
    };

    "a module clangd could not find leaves out only the providers that import it"_test = [] {
        // robustness design C2: clangd deadlocks building a module whose imports it cannot resolve (experiments S2, S6),
        // and nothing else stops it answering, so the providers above that module go and every other unit stays.
        const std::map<std::string, std::string> sources {
            { "/p/src/base.cppm", "export module base;\n" },
            { "/p/src/mid.cppm", "export module mid;\nimport base;\n" },
            { "/p/src/top.cppm", "export module top;\nimport mid;\n" },
            { "/p/src/main.cpp", "import top;\nimport other;\nint main() {}\n" },
            { "/p/src/impl.cpp", "module mid;\nint helper() { return 1; }\n" },
            { "/p/src/other.cppm", "export module other;\n" },
        };
        s::Database database;
        database.hasIde = true;
        s::Set set;
        set.name = "app";
        set.hasIde = true;
        set.toolchain = "gcc-16.1.0-x86_64-linux-gnu";
        for (const auto& [path, text] : sources) {
            s::TranslationUnit unit;
            unit.source = path;
            unit.workDirectory = "/p";
            unit.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-c", path };
            set.units.push_back(std::move(unit));
        }
        database.sets.push_back(set);
        std::map<std::string, ToolchainFacts, std::less<>> facts { { set.toolchain, gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        input.unresolvedModules = { { "base", "Don't get the module unit for module base" } };
        const auto plan = n::plan_engine(input);
        std::set<std::string> files;
        for (const auto& entry : plan.entries) files.insert(entry.file);
        expect(files.contains("/p/src/base.cppm")) << "the unit itself imports nothing";
        expect(!files.contains("/p/src/mid.cppm") && !files.contains("/p/src/top.cppm")) << "providers above it cannot be built";
        expect(files.contains("/p/src/main.cpp") && files.contains("/p/src/impl.cpp")) << "units that provide nothing stay";
        expect(files.contains("/p/src/other.cppm")) << "an unrelated module is untouched";
        expect(plan.excludedFiles.size() == 2u) << plan.excludedFiles.size();
    };

    "C++ units are read with the semantic kit when the toolchain's std could not be built"_test = [] {
        const std::map<std::string, std::string> sources {
            { "/p/src/main.cpp", "import std;\nint main() {}\n" },
            { "/p/src/zlib.c", "int crc(void) { return 0; }\n" },
        };
        s::Database database;
        database.hasIde = true;
        s::Set set;
        set.name = "app";
        set.hasIde = true;
        set.toolchain = "gcc-16.1.0-x86_64-linux-gnu";
        s::TranslationUnit cxx;
        cxx.source = "/p/src/main.cpp";
        cxx.workDirectory = "/p";
        cxx.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-I/p/include", "-c", cxx.source };
        s::TranslationUnit c;
        c.source = "/p/src/zlib.c";
        c.workDirectory = "/p";
        c.arguments = { "/opt/gcc/bin/gcc", "-std=c11", "-c", c.source };
        set.units = { cxx, c };
        database.sets.push_back(set);
        std::map<std::string, ToolchainFacts, std::less<>> facts { { set.toolchain, gcc_facts() } };
        s::Kit kit;
        kit.target = "x86_64-unknown-linux-gnu";
        kit.moduleMetadata = "/kit/lib/libc++.modules.json";
        kit.arguments = { "-nostdinc++" };
        kit.systemIncludeDirectories = { "/kit/include/c++/v1" };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.kit = &kit;
        input.preferKit = true;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        input.metadataReader = [](std::string_view manifest) {
            if (manifest == "/kit/lib/libc++.modules.json") return std::vector<s::ModuleEntry> { { "std", "/kit/share/libc++/v1/std.cppm", true, { "/kit/share/libc++/v1" }, {} } };
            return std::vector<s::ModuleEntry> { { "std", "/opt/gcc/include/c++/16/bits/std.cc", true, {}, {} } };
        };
        const auto plan = n::plan_engine(input);
        const auto entry_of = [&](std::string_view file) -> const n::EngineEntry* {
            const auto it = std::ranges::find_if(plan.entries, [&](const n::EngineEntry& entry) { return entry.file == file; });
            return it == plan.entries.end() ? nullptr : &*it;
        };
        const auto* main = entry_of("/p/src/main.cpp");
        expect(fatal(main != nullptr));
        expect(contains(main->arguments, "-nostdinc++") && contains(main->arguments, "-I/p/include") && !contains_prefix(main->arguments, "--gcc-install-dir"))
            << std::format("{}", main->arguments);
        expect(entry_of("/kit/share/libc++/v1/std.cppm") != nullptr) << "std comes from the kit";
        expect(entry_of("/opt/gcc/include/c++/16/bits/std.cc") == nullptr) << "and not from the toolchain";
        const auto* zlib = entry_of("/p/src/zlib.c");
        expect(fatal(zlib != nullptr));
        expect(zlib->arguments.front() == "/payload/clangd/bin/clang" && contains_prefix(zlib->arguments, "--gcc-install-dir")) << "a C unit keeps its toolchain";
    };

    "a kit plan without a toolchain"_test = [] {
        s::Database database;
        s::Set set;
        set.name = "inferred";
        s::TranslationUnit unit;
        unit.source = "/w/main.cpp";
        unit.workDirectory = "/w";
        unit.arguments = { "clang++", "-std=c++23", "-I/w/include", "-c", "/w/main.cpp" };
        set.units.push_back(unit);
        database.sets.push_back(set);
        s::Kit kit;
        kit.target = "x86_64-unknown-linux-gnu";
        kit.moduleMetadata = "/kit/lib/libc++.modules.json";
        kit.arguments = { "-nostdinc++" };
        kit.systemIncludeDirectories = { "/kit/include/c++/v1" };
        kit.sysroot = "/kit/sysroot";
        n::PlanInput input;
        input.database = &database;
        input.kit = &kit;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [](std::string_view) { return p::scan_source("import std;\n"); };
        input.metadataReader = [](std::string_view) {
            return std::vector<s::ModuleEntry> { { "std", "/kit/share/libc++/v1/std.cppm", true, { "/kit/share/libc++/v1" }, {} } };
        };
        const auto plan = n::plan_engine(input);
        expect(fatal(plan.entries.size() == 2u));
        const auto& main = plan.entries[0];
        expect(contains(main.arguments, "--sysroot=/kit/sysroot") && contains(main.arguments, "-I/w/include") && contains(main.arguments, "-std=c++23"));
        expect(std::ranges::count_if(main.arguments, [](const std::string& a) { return a.starts_with("-std="); }) == 1);
        const auto& std = plan.entries[1];
        expect(std.file == "/kit/share/libc++/v1/std.cppm");
        expect(contains(std.arguments, "/kit/share/libc++/v1"));
        expect(plan.issues.empty());
    };

    "an MSVC plan injects the MSVC STL's modules and drops the ones a build compiled"_test = [] {
        const std::string tools { "/VS/VC/Tools/MSVC/14.44.35207" };
        s::Database database;
        s::Set set;
        set.name = "greet";
        set.toolchain = "msvc";
        auto unit = [&](std::string source, std::vector<std::string> arguments) {
            s::TranslationUnit value;
            value.source = std::move(source);
            value.workDirectory = "/p/build";
            value.arguments = std::move(arguments);
            return value;
        };
        const std::string cl { tools + "/bin/Hostx64/x64/cl.exe" };
        set.units.push_back(unit(tools + "/modules/std.ixx", { cl, "/std:c++latest", "/EHsc", "/MDd", "-interface", "-c", tools + "/modules/std.ixx" }));
        set.units.push_back(unit("/p/src/greet.ixx", { cl, "/std:c++latest", "/EHsc", "/MDd", "-c", "/p/src/greet.ixx" }));
        set.units.push_back(unit("/p/src/main.cpp", { cl, "/std:c++latest", "/EHsc", "/MDd", "-c", "/p/src/main.cpp" }));
        database.sets.push_back(set);
        auto msvc = msvc_facts(s::Family::msvc);
        msvc.toolchain.stdlib->moduleMetadata = tools + "/modules/modules.json";
        std::map<std::string, ToolchainFacts, std::less<>> facts { { "msvc", msvc } };
        const std::map<std::string, std::string> sources {
            { tools + "/modules/std.ixx", "module;\n#include <vector>\nexport module std;\n" },
            { "/p/src/greet.ixx", "export module greet;\nimport std;\n" },
            { "/p/src/main.cpp", "import greet;\nimport std;\nint main() {}\n" },
        };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        input.metadataReader = [&](std::string_view manifest) {
            expect(manifest == tools + "/modules/modules.json");
            auto entries = s::parse_module_metadata(nlohmann::json::parse(R"({"version":1,"revision":0,"library":"microsoft/STL","module-sources":["std.ixx","std.compat.ixx"]})"),
                                                    tools + "/modules");
            return entries ? *entries : std::vector<s::ModuleEntry> {};
        };
        const auto plan = n::plan_engine(input);
        expect(plan.issues.empty()) << (plan.issues.empty() ? "" : plan.issues.front().message);
        expect(plan.stdUnits == 2u);
        int stdEntries { 0 };
        for (const auto& entry : plan.entries) {
            expect(entry.arguments.front() == "/payload/clangd/bin/clang++") << entry.arguments.front();
            if (entry.file.ends_with("std.ixx")) {
                ++stdEntries;
                expect(contains(entry.arguments, "-Wno-include-angled-in-module-purview") && contains(entry.arguments, "-fno-aligned-allocation"));
                expect(!contains(entry.arguments, "-interface"));
                expect(entry.arguments[entry.arguments.size() - 3] == "-x" && entry.arguments[entry.arguments.size() - 2] == "c++-module");
            }
            if (entry.file == "/p/src/greet.ixx") expect(contains(entry.arguments, "c++-module"));
        }
        expect(stdEntries == 1) << "the build's std.ixx unit is replaced by the injected one";
    };

    "options decide a unit's semantics when the database has them"_test = [] {
        s::SemanticOptions setOptions;
        setOptions.languageStandard = "c++23";
        setOptions.macros = { { "FROM_SET", "1", false } };
        setOptions.includeDirectories.system = { "/sdk/include" };
        setOptions.rawSemanticArguments = { { "clang", { "-nostdinc++" } }, { "msvc", { "/Zc:__cplusplus" } } };
        s::SemanticOptions unitOptions;
        unitOptions.languageStandard = "c++26";
        unitOptions.macros = { { "FROM_SET", std::nullopt, true } };
        unitOptions.includeDirectories.system = { "/unit/include" };
        unitOptions.rtti = false;
        const auto merged = n::effective_options(setOptions, unitOptions);
        expect(fatal(merged.has_value()));
        expect(merged->languageStandard == std::optional<std::string> { "c++26" }) << "a scalar takes the unit's value";
        expect(merged->macros.size() == 2u && merged->macros[0].name == "FROM_SET" && merged->macros[1].undefine) << "arrays concatenate, the set's first";
        expect(merged->includeDirectories.system == std::vector<std::string> { "/sdk/include", "/unit/include" });
        expect(n::effective_options(setOptions, std::nullopt).has_value() && !n::effective_options(std::nullopt, std::nullopt).has_value());

        const auto gnu = n::options_arguments(*merged, s::Family::clang, "/llvm/bin/clang++", "/p/a.cpp");
        expect(gnu.front() == "/llvm/bin/clang++" && gnu.back() == "/p/a.cpp");
        expect(contains(gnu, "-std=c++26") && contains(gnu, "-DFROM_SET=1") && contains(gnu, "-UFROM_SET") && contains(gnu, "-fno-rtti") && contains(gnu, "-nostdinc++"));
        expect(!contains(gnu, "/Zc:__cplusplus")) << "raw arguments of another family stay out";
        const auto cl = n::options_arguments(*merged, s::Family::msvc, "cl.exe", "C:/p/a.cpp");
        expect(contains(cl, "/std:c++latest") && contains(cl, "/DFROM_SET=1") && contains(cl, "/GR-") && contains(cl, "/external:I") && contains(cl, "/Zc:__cplusplus"));

        // In a plan the options win over what the arguments say.
        s::Database database;
        s::Set set;
        set.name = "app";
        set.toolchain = "gcc";
        set.options = setOptions;
        s::TranslationUnit unit;
        unit.source = "/p/src/main.cpp";
        unit.workDirectory = "/p";
        unit.arguments = { "/opt/gcc/bin/g++", "-std=c++20", "-DFROM_ARGUMENTS", "-c", "/p/src/main.cpp" };
        set.units.push_back(unit);
        database.sets.push_back(set);
        std::map<std::string, ToolchainFacts, std::less<>> facts { { "gcc", gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [](std::string_view) { return p::scan_source("int main() {}\n"); };
        const auto plan = n::plan_engine(input);
        expect(fatal(plan.entries.size() == 1u));
        const auto& arguments = plan.entries.front().arguments;
        expect(contains(arguments, "-DFROM_SET=1") && contains(arguments, "-std=c++23"));
        expect(!contains(arguments, "-DFROM_ARGUMENTS") && !contains(arguments, "-std=c++20"));

        // Options the S1 library derived restate the arguments: the arguments are what is compiled.
        database.sets.front().optionsDerived = true;
        const auto derived = n::plan_engine(input);
        expect(fatal(derived.entries.size() == 1u));
        const auto& restated = derived.entries.front().arguments;
        expect(contains(restated, "-DFROM_ARGUMENTS") && contains(restated, "-std=c++20"));
        expect(!contains(restated, "-DFROM_SET=1"));
    };

    "a kit plan excludes std when the macOS SDK is missing"_test = [] {
        // usable plan W5.4 / experiment E15: without the SDK a kit that needs it cannot build std;
        // the importing unit leaves the engine database with an sdk-missing issue instead of being
        // sent to clangd, where it would never answer. A unit that does not need std is unaffected.
        s::Database database;
        s::Set set;
        set.name = "inferred";
        auto unit = [](std::string source, std::vector<std::string> arguments) {
            s::TranslationUnit value;
            value.source = std::move(source);
            value.workDirectory = "/w";
            value.arguments = std::move(arguments);
            return value;
        };
        set.units.push_back(unit("/w/uses-std.cpp", { "clang++", "-std=c++23", "-c", "/w/uses-std.cpp" }));
        set.units.push_back(unit("/w/plain.cpp", { "clang++", "-std=c++23", "-c", "/w/plain.cpp" }));
        database.sets.push_back(set);
        s::Kit kit;
        kit.target = "arm64-apple-macos11";
        kit.moduleMetadata = "/kit/lib/libc++.modules.json";
        kit.systemIncludeDirectories = { "/kit/include/c++/v1" };
        kit.requirements = { "macos-sdk" };
        const std::map<std::string, std::string> sources {
            { "/w/uses-std.cpp", "import std;\nint main() {}\n" },
            { "/w/plain.cpp", "int f() { return 0; }\n" },
        };
        n::PlanInput input;
        input.database = &database;
        input.kit = &kit;
        input.macosSdk.clear();   // not found: this test does not need a macOS machine to prove it
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        input.metadataReader = [](std::string_view) {
            return std::vector<s::ModuleEntry> { { "std", "/kit/share/libc++/v1/std.cppm", true, { "/kit/share/libc++/v1" }, {} } };
        };
        const auto plan = n::plan_engine(input);
        std::set<std::string> files;
        for (const auto& entry : plan.entries) files.insert(entry.file);
        expect(!files.contains("/w/uses-std.cpp")) << "left out: it cannot build without the SDK";
        expect(files.contains("/w/plain.cpp")) << "unaffected: it does not need std";
        expect(plan.excludedFiles.size() == 1u && plan.excludedFiles.front() == "/w/uses-std.cpp");
        expect(plan.stdUnits == 0u) << "std itself is not injected: nothing would import it successfully";
        const auto has_issue = [&](std::string_view code) {
            return std::ranges::any_of(plan.issues, [&](const n::PlanIssue& issue) { return issue.code == code; });
        };
        expect(has_issue("sdk-missing"));

        // Once the SDK is available, the same workspace builds std normally.
        input.macosSdk = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";
        const auto withSdk = n::plan_engine(input);
        std::set<std::string> filesWithSdk;
        for (const auto& entry : withSdk.entries) filesWithSdk.insert(entry.file);
        expect(filesWithSdk.contains("/w/uses-std.cpp") && filesWithSdk.contains("/w/plain.cpp"));
        expect(withSdk.stdUnits == 1u);
        expect(std::ranges::none_of(withSdk.issues, [](const n::PlanIssue& issue) { return issue.code == "sdk-missing"; }));
    };

    "prime units and module hints"_test = [] {
        const std::map<std::string, std::string> sources {
            { "/p/src/main.cpp", "import std;\nimport app;\nint main() {}\n" },
            { "/p/src/app.cppm", "export module app;\nexport import :part;\nimport lib;\n" },
            { "/p/src/part.cppm", "export module app:part;\nimport lib;\n" },
            { "/p/src/lib.cppm", "export module lib;\nimport std;\n" },
            { "/p/src/lib.cpp", "module lib;\n" },
        };
        s::Database database;
        s::Set set;
        set.name = "app";
        set.toolchain = "gcc";
        for (const auto& [path, text] : sources) {
            s::TranslationUnit unit;
            unit.source = path;
            unit.workDirectory = "/p";
            unit.arguments = { "/opt/gcc/bin/g++", "-std=c++23", "-fmodules", "-c", path };
            set.units.push_back(std::move(unit));
        }
        database.sets.push_back(set);
        std::map<std::string, ToolchainFacts, std::less<>> facts { { "gcc", gcc_facts() } };
        n::PlanInput input;
        input.database = &database;
        input.facts = &facts;
        input.engineDriverDirectory = "/payload/clangd/bin";
        input.scanner = [&](std::string_view path) {
            const auto it = sources.find(std::string { path });
            return it == sources.end() ? p::ScanResult {} : p::scan_source(it->second);
        };
        input.metadataReader = [](std::string_view) {
            return std::vector<s::ModuleEntry> { { "std", "/opt/gcc/include/c++/16/bits/std.cc", true, {}, {} },
                                                 { "std.compat", "/opt/gcc/include/c++/16/bits/std.compat.cc", true, {}, {} } };
        };
        input.primeDirectory = "/cache/prime";
        input.moduleHintDirectory = "/cache/hints";
        const auto plan = n::plan_engine(input);
        expect(plan.issues.empty()) << (plan.issues.empty() ? "" : plan.issues.front().message);

        const auto hint = [](std::string_view file) { return mcppls::base::join_path("/cache/hints", file); };
        const auto entry_for = [&](std::string_view file) -> const n::EngineEntry* {
            const auto it = std::ranges::find_if(plan.entries, [&](const n::EngineEntry& entry) { return entry.file == file; });
            return it == plan.entries.end() ? nullptr : &*it;
        };
        const auto* main = entry_for("/p/src/main.cpp");
        expect(fatal(main != nullptr));
        expect(main->provides.empty());
        expect(contains(main->moduleHints, "-fmodule-file=app=" + hint("app.pcm")));
        expect(contains(main->moduleHints, "-fmodule-file=app:part=" + hint("app-part.pcm"))) << "a module reached through another is named too";
        expect(contains(main->moduleHints, "-fmodule-file=lib=" + hint("lib.pcm")));
        expect(contains(main->moduleHints, "-fmodule-file=std=" + hint("std.pcm")));
        expect(!contains_prefix(main->moduleHints, "-fmodule-output=")) << "a unit that provides nothing produces nothing";

        const auto* app = entry_for("/p/src/app.cppm");
        expect(fatal(app != nullptr));
        expect(app->provides == "app");
        expect(contains(app->moduleHints, "-fmodule-output=" + hint("app.pcm")));
        expect(contains(app->moduleHints, "-fmodule-file=lib=" + hint("lib.pcm")) && contains(app->moduleHints, "-fmodule-file=std=" + hint("std.pcm")));
        expect(!contains(app->moduleHints, "-fmodule-file=app=" + hint("app.pcm"))) << "a module does not reach itself";
        const auto* part = entry_for("/p/src/part.cppm");
        expect(fatal(part != nullptr));
        expect(contains(part->moduleHints, "-fmodule-output=" + hint("app-part.pcm")));
        const auto* implementation = entry_for("/p/src/lib.cpp");
        expect(fatal(implementation != nullptr));
        expect(contains(implementation->moduleHints, "-fmodule-file=lib=" + hint("lib.pcm"))) << "an implementation unit reaches its interface";
        expect(!contains_prefix(implementation->moduleHints, "-fmodule-output="));
        const auto* compat = entry_for("/opt/gcc/include/c++/16/bits/std.compat.cc");
        expect(fatal(compat != nullptr));
        expect(contains(compat->moduleHints, "-fmodule-output=" + hint("std.compat.pcm")) && contains(compat->moduleHints, "-fmodule-file=std=" + hint("std.pcm")));

        // Prime units: one per importable module except partitions, each importing its module.
        std::set<std::string> primed;
        for (const auto& module : plan.modules) {
            if (module.name == "app:part") expect(module.primeFile.empty()) << "a partition is built through its primary module";
            if (module.name == "app") expect(contains(module.requires_, "lib") && contains(module.requires_, "app:part"));
            if (!module.primeFile.empty()) primed.insert(module.name);
        }
        expect(primed == std::set<std::string> { "app", "lib", "std", "std.compat" });
        for (const auto& [file, content] : plan.primeSources) {
            const auto* prime = entry_for(file);
            expect(fatal(prime != nullptr));
            expect(fatal(prime->imports.size() == 1u));
            expect(content == std::format("import {};\n", prime->imports.front()));
            expect(contains(prime->moduleHints, std::format("-fmodule-file={}={}", prime->imports.front(), hint(prime->imports.front() + ".pcm"))));
            expect(!contains(prime->arguments, "c++-module")) << "a prime unit is not a module unit";
        }

        // Hints are written before the source, and a database without them is the plan's structure.
        const auto written = n::to_compile_commands(plan);
        const auto structure = n::to_compile_commands(plan, false);
        for (std::size_t i { 0 }; i < plan.entries.size(); ++i) {
            const auto arguments = written[i]["arguments"].get<std::vector<std::string>>();
            expect(arguments.back() == plan.entries[i].file);
            expect(arguments.size() == plan.entries[i].arguments.size() + plan.entries[i].moduleHints.size());
            expect(structure[i]["arguments"].get<std::vector<std::string>>() == plan.entries[i].arguments);
        }

        // Without a hint directory the plan has none.
        input.moduleHintDirectory.clear();
        const auto plain = n::plan_engine(input);
        expect(std::ranges::all_of(plain.entries, [](const n::EngineEntry& entry) { return entry.moduleHints.empty(); }));
    };

    return report();
}
