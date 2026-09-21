// mcppls.pack.kit's pure and file-local pieces, ported from packaging/scripts/build_kit.py: the
// two recipes' archive selections, the cmake/ninja argv each recipe built, and write_kit_json's
// checks (every include directory, sysroot and license must exist; the module manifest must
// resolve inside the kit and provide `std`). None of this runs cmake, ninja or dpkg, or reaches
// the network -- the recipes that do are exercised by hand in the parity check (tooling
// architecture M2), not here.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.kit;
import mcppls.platform.fs;
import nlohmann.json;

namespace kit = mcppls::pack::kit;
namespace base = mcppls::base;
namespace fs = mcppls::platform::fs;

namespace {

// Beside the build, not the system temp -- the same reason test_archive.cpp gives for it.
std::string temp_dir(std::string_view name) {
    const auto root = std::filesystem::current_path() / ".test-scratch"
                      / std::format("mcppls-kit-{}-{}", name, std::random_device {}());
    std::filesystem::create_directories(root);
    return root.string();
}

void write(const std::string& path, std::string_view content) {
    std::filesystem::create_directories(std::filesystem::path { path }.parent_path());
    auto written = fs::write_file(path, content);
    (void) written;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    // ---- file selection: recipe_libcxx_source's SOURCE_SUBTREES prefix test -------------------

    "the libcxx-source recipe keeps only the six runtime subtrees"_test = [] {
        expect(kit::testing::wants_source_subtree("runtimes/CMakeLists.txt"));
        expect(kit::testing::wants_source_subtree("libcxx/include/vector"));
        expect(kit::testing::wants_source_subtree("libcxxabi/src/abort_message.cpp"));
        expect(kit::testing::wants_source_subtree("llvm/cmake/modules/x.cmake"));
        expect(kit::testing::wants_source_subtree("llvm/utils/llvm-lit/lit.py"));
        expect(kit::testing::wants_source_subtree("cmake/Modules/x.cmake"));
        expect(!kit::testing::wants_source_subtree("clang/lib/Sema/Sema.cpp"));
        expect(!kit::testing::wants_source_subtree("llvm/lib/IR/Core.cpp"));   // llvm/ itself, not llvm/cmake or llvm/utils/llvm-lit
        expect(!kit::testing::wants_source_subtree("README.md"));
    };

    // ---- file selection: the llvm-mingw recipe's `select()` ------------------------------------

    "the llvm-mingw recipe keeps headers, module sources, the manifest and the licenses"_test = [] {
        expect(kit::testing::wants_mingw_member("generic-w64-mingw32/include/stdio.h"));
        expect(kit::testing::wants_mingw_member("generic-w64-mingw32/include/c++/v1/vector"));
        expect(!kit::testing::wants_mingw_member("generic-w64-mingw32/include/windows.idl"));
        expect(!kit::testing::wants_mingw_member("generic-w64-mingw32/include/oaidl.tlb"));
        expect(!kit::testing::wants_mingw_member("generic-w64-mingw32/include/thing.def"));
        expect(kit::testing::wants_mingw_member("share/libc++/v1/std.cppm"));
        expect(kit::testing::wants_mingw_member("x86_64-w64-mingw32/lib/libc++.modules.json"));
        expect(kit::testing::wants_mingw_member("LICENSE.TXT"));
        expect(kit::testing::wants_mingw_member("x86_64-w64-mingw32/share/mingw32/COPYING"));
        expect(kit::testing::wants_mingw_member("x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt"));
        expect(!kit::testing::wants_mingw_member("x86_64-w64-mingw32/share/mingw32/README"));   // not a COPYING* name
        expect(!kit::testing::wants_mingw_member("x86_64-w64-mingw32/bin/clang.exe"));
        expect(!kit::testing::wants_mingw_member("aarch64-w64-mingw32/include/stdio.h"));   // a different target triple
    };

    "glob_match covers fnmatch's '*' and '?' the way MINGW_EXCLUDED needs"_test = [] {
        expect(kit::testing::glob_match("windows.idl", "*.idl"));
        expect(kit::testing::glob_match("a.tlb", "*.tlb"));
        expect(kit::testing::glob_match("thing.def", "*.def"));
        expect(!kit::testing::glob_match("windows.idl", "*.tlb"));
        expect(!kit::testing::glob_match("idl", "*.idl"));   // no name before the dot: fnmatch still needs the literal "."
        expect(kit::testing::glob_match(".idl", "*.idl"));
        expect(kit::testing::glob_match("a.h", "?.h"));
        expect(!kit::testing::glob_match("ab.h", "?.h"));
    };

    // ---- argument construction: the cmake configure argv and the ninja install argv -----------

    "linux-x64's configure argv adds the per-target-runtime-dir flags, not the OSX ones"_test = [] {
        const kit::testing::ConfigureInputs inputs {
            .platform = "linux-x64", .target = "x86_64-unknown-linux-gnu",
            .runtimesDir = "/src/llvm-project/runtimes", .buildDir = "/work/build", .stageDir = "/work/stage",
            .ninja = "/usr/bin/ninja", .cc = "/usr/bin/clang", .cxx = "/usr/bin/clang++",
        };
        const auto args = kit::testing::configure_arguments(inputs);
        auto has = [&](std::string_view needle) { return std::ranges::find(args, std::string { needle }) != args.end(); };
        expect(has("-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON"));
        expect(has("-DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-linux-gnu"));
        expect(!has("-DCMAKE_OSX_ARCHITECTURES=arm64"));
        expect(has("-S"));
        expect(has("/src/llvm-project/runtimes"));
        expect(has("-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja"));
        expect(has("-DCMAKE_C_COMPILER=/usr/bin/clang"));
        expect(has("-DCMAKE_CXX_COMPILER=/usr/bin/clang++"));
        expect(has("-DCMAKE_INSTALL_PREFIX=/work/stage"));
        expect(has("-DLIBCXX_INSTALL_MODULES=ON"));
    };

    "darwin-arm64's configure argv adds the OSX architecture flag instead"_test = [] {
        const kit::testing::ConfigureInputs inputs {
            .platform = "darwin-arm64", .target = "arm64-apple-darwin",
            .runtimesDir = "/src/llvm-project/runtimes", .buildDir = "/work/build", .stageDir = "/work/stage",
            .ninja = "/usr/bin/ninja", .cc = "/usr/bin/clang", .cxx = "/usr/bin/clang++",
        };
        const auto args = kit::testing::configure_arguments(inputs);
        auto has = [&](std::string_view needle) { return std::ranges::find(args, std::string { needle }) != args.end(); };
        expect(has("-DCMAKE_OSX_ARCHITECTURES=arm64"));
        expect(!has("-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON"));
        expect(!std::ranges::any_of(args, [](const std::string& a) { return a.starts_with("-DLLVM_DEFAULT_TARGET_TRIPLE"); }));
    };

    "the ninja install argv has no -j when no --jobs was given"_test = [] {
        const auto args = kit::testing::install_arguments("/work/build", std::nullopt);
        expect((args == std::vector<std::string> { "-C", "/work/build", "install-cxx-headers",
                                                    "install-cxxabi-headers", "install-cxx-modules" }));
    };

    "the ninja install argv carries -j right after -C when --jobs was given"_test = [] {
        const auto args = kit::testing::install_arguments("/work/build", 8);
        expect((args == std::vector<std::string> { "-C", "/work/build", "-j", "8", "install-cxx-headers",
                                                    "install-cxxabi-headers", "install-cxx-modules" }));
    };

    // ---- write_kit_json: every check build_kit.py's write_kit_json and check_manifest made ----

    "write_kit_json accepts a manifest whose paths already resolve inside the kit"_test = [&] {
        const std::string kitDir { temp_dir("ok") };
        write(base::join_path(kitDir, "include/c++/v1/vector"), "// header");
        write(base::join_path(kitDir, "licenses/LICENSE.txt"), "license text");
        write(base::join_path(kitDir, "share/libc++/v1/std.cppm"), "// std module");
        const std::string manifestRel { "lib/x86_64-unknown-linux-gnu/libc++.modules.json" };
        const std::string manifestText {
            R"({"version":1,"revision":1,"modules":[)"
            R"({"logical-name":"std","source-path":"../../share/libc++/v1/std.cppm","is-std-library":true,)"
            R"("local-arguments":{"system-include-directories":["../../share/libc++/v1"]}}]})"
        };
        write(base::join_path(kitDir, manifestRel), manifestText);

        nlohmann::ordered_json data;
        data["kit-version"] = 1;
        data["name"] = "mcppls-kit-libcxx-test-x86_64-unknown-linux-gnu";
        data["target"] = "x86_64-unknown-linux-gnu";
        data["stdlib"] = { { "name", "libc++" }, { "version", "test" }, { "module-metadata", manifestRel } };
        data["system-include-directories"] = std::vector<std::string> { "include/c++/v1" };
        data["sysroot"] = nullptr;
        data["arguments"] = std::vector<std::string> { "-nostdinc++" };
        data["licenses"] = std::vector<std::string> { "licenses/LICENSE.txt" };

        auto written = kit::testing::write_kit_json(kitDir, data);
        expect(written.has_value()) << (written ? std::string {} : written.error().message);

        auto kitJson = fs::read_file(base::join_path(kitDir, "kit.json"));
        expect(kitJson.has_value());
        // write_kit_json's own output must be exactly data.dump(2) + "\n" -- the parity check
        // (tooling architecture M2) depends on this being the whole story, byte for byte.
        expect(*kitJson == data.dump(2) + "\n");

        // The manifest already resolved, so it must be untouched -- no relocation, no rewrite.
        auto manifestOnDisk = fs::read_file(base::join_path(kitDir, manifestRel));
        expect(manifestOnDisk.has_value());
        expect(*manifestOnDisk == manifestText);

        std::filesystem::remove_all(kitDir);
    };

    "write_kit_json relocates a source-path to share/libc++/v1 and rewrites the manifest"_test = [&] {
        const std::string kitDir { temp_dir("relocate") };
        write(base::join_path(kitDir, "include/c++/v1/vector"), "// header");
        write(base::join_path(kitDir, "licenses/LICENSE.txt"), "license text");
        // The manifest names a source-path relative to its own directory that does not exist
        // there; the fallback of the same basename under share/libc++/v1 does.
        write(base::join_path(kitDir, "share/libc++/v1/std.cppm"), "// std module");
        const std::string manifestRel { "lib/x86_64-unknown-linux-gnu/libc++.modules.json" };
        write(base::join_path(kitDir, manifestRel),
             R"({"modules":[{"logical-name":"std","source-path":"std.cppm"}]})");

        nlohmann::ordered_json data;
        data["stdlib"] = { { "name", "libc++" }, { "version", "test" }, { "module-metadata", manifestRel } };
        data["system-include-directories"] = std::vector<std::string> { "include/c++/v1" };
        data["sysroot"] = nullptr;
        data["licenses"] = std::vector<std::string> { "licenses/LICENSE.txt" };

        auto written = kit::testing::write_kit_json(kitDir, data);
        expect(written.has_value()) << (written ? std::string {} : written.error().message);

        auto manifestOnDisk = fs::read_file(base::join_path(kitDir, manifestRel));
        expect(manifestOnDisk.has_value());
        if (manifestOnDisk) {
            const nlohmann::json rewritten = nlohmann::json::parse(*manifestOnDisk, nullptr, false);
            expect(!rewritten.is_discarded());
            expect(rewritten["modules"][0]["source-path"].get<std::string>() == "../../share/libc++/v1/std.cppm");
        }

        std::filesystem::remove_all(kitDir);
    };

    "write_kit_json fails when a source-path resolves nowhere in the kit"_test = [&] {
        const std::string kitDir { temp_dir("dangling") };
        write(base::join_path(kitDir, "include/c++/v1/vector"), "// header");
        write(base::join_path(kitDir, "licenses/LICENSE.txt"), "license text");
        const std::string manifestRel { "lib/x86_64-unknown-linux-gnu/libc++.modules.json" };
        write(base::join_path(kitDir, manifestRel),
             R"({"modules":[{"logical-name":"std","source-path":"nowhere.cppm"}]})");

        nlohmann::ordered_json data;
        data["stdlib"] = { { "name", "libc++" }, { "version", "test" }, { "module-metadata", manifestRel } };
        data["system-include-directories"] = std::vector<std::string> { "include/c++/v1" };
        data["sysroot"] = nullptr;
        data["licenses"] = std::vector<std::string> { "licenses/LICENSE.txt" };

        auto written = kit::testing::write_kit_json(kitDir, data);
        expect(!written.has_value());
        if (!written) expect(written.error().code == "kit-manifest");

        std::filesystem::remove_all(kitDir);
    };

    "write_kit_json fails when the manifest has no std module"_test = [&] {
        const std::string kitDir { temp_dir("no-std") };
        write(base::join_path(kitDir, "include/c++/v1/vector"), "// header");
        write(base::join_path(kitDir, "licenses/LICENSE.txt"), "license text");
        write(base::join_path(kitDir, "share/libc++/v1/std.compat.cppm"), "// std.compat module");
        const std::string manifestRel { "lib/x86_64-unknown-linux-gnu/libc++.modules.json" };
        write(base::join_path(kitDir, manifestRel),
             R"({"modules":[{"logical-name":"std.compat","source-path":"../../share/libc++/v1/std.compat.cppm"}]})");

        nlohmann::ordered_json data;
        data["stdlib"] = { { "name", "libc++" }, { "version", "test" }, { "module-metadata", manifestRel } };
        data["system-include-directories"] = std::vector<std::string> { "include/c++/v1" };
        data["sysroot"] = nullptr;
        data["licenses"] = std::vector<std::string> { "licenses/LICENSE.txt" };

        auto written = kit::testing::write_kit_json(kitDir, data);
        expect(!written.has_value());
        if (!written) expect(written.error().code == "kit-manifest");

        std::filesystem::remove_all(kitDir);
    };

    "write_kit_json fails when an include directory is missing"_test = [&] {
        const std::string kitDir { temp_dir("missing-include") };
        write(base::join_path(kitDir, "licenses/LICENSE.txt"), "license text");

        nlohmann::ordered_json data;
        data["stdlib"] = { { "name", "libc++" }, { "version", "test" }, { "module-metadata", "lib/x/libc++.modules.json" } };
        data["system-include-directories"] = std::vector<std::string> { "include/c++/v1" };   // never created
        data["sysroot"] = nullptr;
        data["licenses"] = std::vector<std::string> { "licenses/LICENSE.txt" };

        auto written = kit::testing::write_kit_json(kitDir, data);
        expect(!written.has_value());
        if (!written) expect(written.error().code == "kit-missing");

        std::filesystem::remove_all(kitDir);
    };

    "write_kit_json fails when a license file is missing"_test = [&] {
        const std::string kitDir { temp_dir("missing-license") };
        write(base::join_path(kitDir, "include/c++/v1/vector"), "// header");

        nlohmann::ordered_json data;
        data["stdlib"] = { { "name", "libc++" }, { "version", "test" }, { "module-metadata", "lib/x/libc++.modules.json" } };
        data["system-include-directories"] = std::vector<std::string> { "include/c++/v1" };
        data["sysroot"] = nullptr;
        data["licenses"] = std::vector<std::string> { "licenses/does-not-exist.txt" };

        auto written = kit::testing::write_kit_json(kitDir, data);
        expect(!written.has_value());
        if (!written) expect(written.error().code == "kit-missing");

        std::filesystem::remove_all(kitDir);
    };

    "write_kit_json fails when a non-null sysroot directory is missing"_test = [&] {
        const std::string kitDir { temp_dir("missing-sysroot") };
        write(base::join_path(kitDir, "include/c++/v1/vector"), "// header");
        write(base::join_path(kitDir, "licenses/LICENSE.txt"), "license text");

        nlohmann::ordered_json data;
        data["stdlib"] = { { "name", "libc++" }, { "version", "test" }, { "module-metadata", "lib/x/libc++.modules.json" } };
        data["system-include-directories"] = std::vector<std::string> { "include/c++/v1" };
        data["sysroot"] = "sysroot";   // never created
        data["licenses"] = std::vector<std::string> { "licenses/LICENSE.txt" };

        auto written = kit::testing::write_kit_json(kitDir, data);
        expect(!written.has_value());
        if (!written) expect(written.error().code == "kit-missing");

        std::filesystem::remove_all(kitDir);
    };

    return report();
}
