import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.project.compdb;
import mcppls.project.target;
import mcppls.project.boundary;
import mcppls.project.model;
import mcppls.project.modelcache;

namespace p = mcppls::project;
namespace fs = mcppls::platform::fs;
namespace b = mcppls::base;

int main() {
    using namespace mcppls::testing;
    const std::string root { b::join_path(mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-target-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)fs::create_directories(root);
    auto command = [&](std::string file, std::vector<std::string> args) {
        return p::CompileCommand { root, b::join_path(root, file), {}, std::move(args) };
    };

    "bounded reads reject overflow without trusting a preceding file stamp"_test = [&] {
        const std::string path { b::join_path(root, "bounded.txt") };
        (void)fs::write_file(path, "12345678");
        const auto exact = fs::read_file(path, 8);
        expect(exact && *exact == "12345678");
        const auto overflow = fs::read_file(path, 7);
        expect(!overflow && overflow.error().code == "read-file-limit");
        (void)fs::write_file(path, "");
        expect(fs::read_file(path, 0).has_value());
        (void)fs::write_file(path, std::string(65536, 'x'));
        expect(!fs::read_file(path, 8).has_value()) << "growth since an earlier stamp cannot expand the read budget";
    };

    "automatic selection requires every project command to agree"_test = [&] {
        std::vector<p::CompileCommand> commands {
            command("src/中文.cpp", {"clang++", "--target=i686-windows-msvc", "-c", "src/中文.cpp"}),
            command("src/second.cpp", {"clang++", "-target", "i686-pc-windows-msvc", "-c", "src/second.cpp"}),
            command("build.mcpp", {"clang++", "--target=x86_64-windows-msvc", "build.mcpp"}),
            { "/outside", "/outside/std.cppm", {}, {"clang++", "--target=x86_64-windows-msvc"} },
        };
        const auto selected = p::target_from_commands(root, commands);
        expect(selected.source == "compile-commands");
        expect(p::equivalent_mcpp_targets(selected.requestedTarget, "i686-windows-msvc"));
        commands.push_back(command("third.cpp", {"clang++", "-c", "third.cpp"}));
        expect(p::target_from_commands(root, commands).requestedTarget.empty());
        commands.back().arguments = {"clang++", "--target=x86_64-windows-msvc"};
        expect(p::target_from_commands(root, commands).requestedTarget.empty());
    };

    "project ownership follows filesystem aliases without changing compiler commands"_test = [&] {
        const std::string physical { b::join_path(root, "identity/project") };
        const std::string external { b::join_path(root, "identity/external.cpp") };
        const auto write = [&](const std::string& path, std::string_view text) {
            expect(fatal(fs::create_directories(b::parent_path(path)).has_value()));
            expect(fatal(fs::write_file(path, text).has_value()));
        };
        write(b::join_path(physical, "mcpp.toml"), "[workspace]\nmembers = [\"members/one\"]\n");
        write(b::join_path(physical, "src/main.cpp"), "int main() {}\n");
        write(b::join_path(physical, "build.mcpp"), "int main() {}\n");
        write(b::join_path(physical, "members/one/mcpp.toml"), "[package]\nname = \"one\"\n");
        write(b::join_path(physical, "members/one/src/one.cpp"), "int one;\n");
        write(b::join_path(physical, "nested/mcpp.toml"), "[package]\nname = \"nested\"\n");
        write(b::join_path(physical, "nested/main.cpp"), "int nested;\n");
        write(external, "int external;\n");
        const std::string canonical { fs::canonical_path(physical) };
        std::string alias { physical }; // Windows TEMP can already contain a nonprivileged 8.3 alias.
        if constexpr (b::NATIVE_PATH_STYLE != b::PathStyle::windows) {
            alias = b::join_path(root, "identity/project-alias");
            std::error_code error;
            std::filesystem::create_directory_symlink(physical, alias, error);
            expect(fatal(!error)) << error.message();
        }
        const std::string source { b::join_path(alias, "src/main.cpp") };
        const p::ProjectBoundaries throughCanonical { canonical };
        const p::ProjectBoundaries throughAlias { alias };
        expect(!throughCanonical.owns(""));
        expect(throughCanonical.owns(source));
        expect(throughCanonical.owns(b::join_path(alias, "src/generated/not-written.cpp")))
            << "a generated source need not exist for its existing parents to establish ownership";
        expect(throughAlias.owns(b::join_path(canonical, "src/main.cpp")));
        expect(throughCanonical.owns(b::join_path(alias, "members/one/src/one.cpp"))) << "declared workspace members remain part of the project";
        expect(!throughCanonical.owns(b::join_path(alias, "nested/main.cpp"))) << "a separate nested project is still excluded";
        expect(!throughCanonical.owns(external));
        if constexpr (b::NATIVE_PATH_STYLE != b::PathStyle::windows) {
            const std::string linkedExternal { b::join_path(physical, "src/external.cpp") };
            std::error_code error;
            std::filesystem::create_symlink(external, linkedExternal, error);
            expect(fatal(!error)) << error.message();
            expect(!throughCanonical.owns(linkedExternal)) << "a symlink to an external source does not make it a project unit";
        }
        const auto unit = [&](const std::string& file, const std::string& target) {
            return p::CompileCommand { alias, file, {}, { "clang++", "--target=" + target, "-c", file } };
        };
        const std::vector<p::CompileCommand> commands {
            unit(source, "i686-windows-msvc"),
            unit(b::join_path(alias, "members/one/src/one.cpp"), "i686-windows-msvc"),
            unit(b::join_path(alias, "build.mcpp"), "x86_64-windows-msvc"),
            unit(b::join_path(alias, "nested/main.cpp"), "x86_64-windows-msvc"),
            unit(external, "x86_64-windows-msvc"),
        };
        const auto selected = p::target_from_commands(canonical, commands);
        expect(selected.source == "compile-commands");
        expect(selected.requestedTarget == "i686-windows-msvc");
        expect(p::target_from_commands(alias, commands).requestedTarget == selected.requestedTarget);
        expect(commands.front().file == source && commands.front().arguments.back() == source)
            << "ownership resolution must not rewrite the published command or source spelling";
    };

    "target-affecting opaque inputs prevent auto selection"_test = [&] {
        for (const std::string& opaque : { "@flags.rsp", "-m16", "-mabi=ilp32", "-mbig-endian", "-m32", "-m64", "-mx32", "-Xclang", "--config=flags.cfg", "-target=x86_64-linux-gnu", "--" }) {
            const std::vector<p::CompileCommand> commands { command("main.cpp", {"clang++", "--target=i686-windows-msvc", opaque}) };
            expect(p::target_from_commands(root, commands).requestedTarget.empty()) << opaque;
        }
        const std::vector<p::CompileCommand> wrapper {command("main.cpp", {"ccache", "clang++", "--target=i686-windows-msvc"})};
        expect(p::target_from_commands(root, wrapper).requestedTarget.empty());
        const std::vector<p::CompileCommand> contradictory {command("main.cpp", {"clang++", "--target=i686-windows-msvc", "-target", "x86_64-windows-msvc"})};
        expect(p::target_from_commands(root, contradictory).requestedTarget.empty());
    };

    "numeric compiler versions remain recognizable without trusting wrappers"_test = [&] {
        for (const std::string& driver : {"clang++-22", "clang-22.1.8", "g++-16", "clang++-22.exe"}) {
            const std::vector<p::CompileCommand> commands {command("main.cpp", {driver, "--target=i686-windows-msvc"})};
            expect(!p::target_from_commands(root, commands).requestedTarget.empty()) << driver;
        }
        const std::vector<p::CompileCommand> wrapped {command("main.cpp", {"clang++-wrapper", "--target=i686-windows-msvc"})};
        expect(p::target_from_commands(root, wrapped).requestedTarget.empty());
    };

    "configuration overrides existing CDB and project mode ignores it"_test = [&] {
        (void)fs::write_file(b::join_path(root, "compile_commands.json"), "invalid");
        expect(p::select_mcpp_target(root).requestedTarget.empty());
        auto selected = p::select_mcpp_target(root, "aarch64-linux-gnu");
        expect(selected.requestedTarget == "aarch64-linux-gnu" && selected.source == "configuration");
        expect(selected.evidencePath.empty()) << "an explicit setting does not claim CDB evidence";
        selected = p::select_mcpp_target(root, "project");
        expect(selected.mode == "project" && selected.requestedTarget.empty());
        expect(selected.evidencePath.empty()) << "project mode does not consult the CDB";
        expect(!p::valid_mcpp_target_selector("--help"));
        expect(!p::valid_mcpp_target_selector("i686 windows"));
        expect(!p::valid_mcpp_target_selector("$(whoami)"));
        expect(p::valid_mcpp_target_selector("future_arch-vendor-os1.0-abi"));
    };

    "CDB parser preserves explicit argv on both platforms"_test = [&] {
        for (const auto syntax : {p::CommandSyntax::windows, p::CommandSyntax::posix}) {
            const nlohmann::json db = nlohmann::json::array({{
                {"directory",root}, {"file","with space.cpp"},
                {"command", "\"clang++\" --target i686-windows-msvc -c \"with space.cpp\""}
            }});
            const auto commands = p::parse_compile_commands(db, syntax);
            expect(commands.has_value());
            if (commands) expect(p::target_from_commands(root, *commands).requestedTarget == "i686-windows-msvc");
        }
    };

    "invalid rows cannot be skipped to manufacture target uniqueness"_test = [&] {
        nlohmann::json db = nlohmann::json::array({{
            {"directory",root}, {"file","main.cpp"}, {"arguments", {"clang++", "--target=i686-windows-msvc"}}
        }});
        (void)fs::write_file(b::join_path(root, "compile_commands.json"), db.dump());
        expect(p::select_mcpp_target(root).requestedTarget == "i686-windows-msvc");
        db.push_back({{"directory", root}, {"file", "second.cpp"}, {"arguments", {17}}});
        (void)fs::write_file(b::join_path(root, "compile_commands.json"), db.dump());
        expect(p::select_mcpp_target(root).requestedTarget.empty());
    };

    "selection identity survives model serialization and old cache is distinguishable"_test = [&] {
        p::ProjectModel model;
        model.root = root;
        model.targetSelection = p::select_mcpp_target(root, "i686-windows-msvc");
        auto json = p::model_to_json(model);
        auto loaded = p::model_from_json(json);
        expect(loaded.has_value());
        if (loaded) expect(loaded->targetSelection == model.targetSelection);
        json.erase("targetSelection");
        loaded = p::model_from_json(json);
        expect(loaded.has_value());
        if (loaded) expect(loaded->targetSelection.mode.empty());
        auto same = model.targetSelection;
        same.requestedTarget = "i686-pc-windows-msvc";
        expect(p::target_selection_key(same) == p::target_selection_key(model.targetSelection));
        same.reason = "different wording";
        same.evidencePath = "/other/source";
        expect(p::target_selection_key(same) == p::target_selection_key(model.targetSelection));
        same.requestedTarget = "x86_64-windows-msvc";
        expect(p::target_selection_key(same) != p::target_selection_key(model.targetSelection));
        expect(p::mcpp_target_matches("aarch64-macos", "arm64-apple-macos14.0"));
        expect(p::mcpp_target_matches("aarch64-macos", "arm64-apple-darwin24.6.0"));
        expect(!p::mcpp_target_matches("x86_64-macos", "arm64-apple-macos14.0"));
        expect(!p::equivalent_mcpp_targets("arm64-apple-macos14.0", "arm64-apple-macos15.0"));
        expect(p::equivalent_mcpp_targets("x86_64-windows-gnu", "x86_64-w64-windows-gnu"));
        expect(p::equivalent_mcpp_targets("i686-windows-msvc", "i686-unknown-windows-msvc"));
        expect(p::equivalent_mcpp_targets("x86_64-windows-msvc", "x86_64-unknown-windows-msvc"));
        expect(p::equivalent_mcpp_targets("aarch64-windows-msvc", "aarch64-unknown-windows-msvc"));
        expect(p::equivalent_mcpp_targets("x86_64-windows-gnu", "x86_64-unknown-windows-gnu"));
        expect(!p::equivalent_mcpp_targets("i686-windows-msvc", "x86_64-windows-msvc"));
        expect(p::equivalent_mcpp_targets("x86_64-linux-gnu", "x86_64-unknown-linux-gnu"));
        expect(p::equivalent_mcpp_targets("aarch64-linux-musl", "aarch64-unknown-linux-musl"));
        expect(!p::equivalent_mcpp_targets("x86_64-linux-gnu", "x86_64-linux-musl"));
        expect(!p::equivalent_mcpp_targets("x86_64-linux", "x86_64-linux-gnu"));
    };

    fs::remove_all(root);
    return report();
}
