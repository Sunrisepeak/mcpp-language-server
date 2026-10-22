import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.base.uri;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.server.session;

using Json = nlohmann::json;
namespace fs = mcppls::platform::fs;
namespace b = mcppls::base;

int main() {
    using namespace mcppls::testing;
    const std::string root { b::join_path(mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-session-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
    const std::string directory { b::join_path(root, "src/greet") };
    const std::string source { b::join_path(directory, "greet.cppm") };
    (void)fs::create_directories(directory);
    (void)fs::write_file(source, "export module hello.greet;\n");
    auto folders = [](const std::string& path) {
        return Json::array({ Json { { "uri", b::path_to_uri(path) }, { "name", b::file_name(path) } } });
    };

    "Zed file worktree falls back to its directory rootUri"_test = [&] {
        const Json params { { "rootUri", b::path_to_uri(directory) }, { "rootPath", directory },
                            { "workspaceFolders", folders(source) } };
        const auto chosen = mcppls::server::workspace_folders(params, root);
        expect(fatal(chosen.size() == 1));
        expect(chosen[0].path == directory);
        expect(chosen[0].uri == b::path_to_uri(directory));
    };

    "a file-only initialize never makes the source its working directory"_test = [&] {
        Json params { { "rootUri", b::path_to_uri(source) }, { "rootPath", source }, { "workspaceFolders", folders(source) } };
        auto chosen = mcppls::server::workspace_folders(params, root);
        expect(fatal(chosen.size() == 1));
        expect(chosen[0].path == root) << "use the existing cwd fallback, without guessing a parent project";
        params["rootPath"] = directory;
        chosen = mcppls::server::workspace_folders(params, root);
        expect(fatal(chosen.size() == 1));
        expect(chosen[0].path == directory);
    };

    "valid and not-yet-created folders retain precedence and spelling"_test = [&] {
        const std::string absent { b::join_path(root, "not-created") };
        Json params { { "rootUri", b::path_to_uri(root) }, { "workspaceFolders", folders(source) } };
        params["workspaceFolders"].push_back(folders(directory)[0]);
        params["workspaceFolders"].push_back(folders(absent)[0]);
        const auto chosen = mcppls::server::workspace_folders(params, root);
        expect(fatal(chosen.size() == 2));
        expect(chosen[0].path == directory);
        expect(chosen[0].uri == params["workspaceFolders"][1]["uri"].get<std::string>());
        expect(chosen[1].path == absent);
        expect(chosen[1].uri == params["workspaceFolders"][2]["uri"].get<std::string>());
    };

    fs::remove_all(root);
    return report();
}
