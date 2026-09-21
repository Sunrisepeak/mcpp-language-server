// Installing into and removing from Zed and CLion, against scratch directories standing in for
// their data directories (mcppls.devtools.editors).
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.pack.archive;
import mcppls.devtools.editors;

namespace editors = mcppls::devtools::editors;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

std::string scratch(std::string_view name) {
    const std::string directory { base::join_path(
        mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-editors-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void) fs::create_directories(directory);
    return directory;
}

// An extension directory as the build leaves it.
std::string zed_source(const std::string& root, std::string_view id = "mcppls") {
    const std::string source { base::join_path(root, "source") };
    (void) fs::create_directories(source);
    (void) fs::write_file(base::join_path(source, "extension.toml"), std::format("id = \"{}\"\nname = \"x\"\n", id));
    (void) fs::write_file(base::join_path(source, "extension.wasm"), "\0asm");
    return source;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "zed: install leaves the extension under installed/, remove takes it and its work directory"_test = [&] {
        const std::string root { scratch("zed") };
        const std::string source { zed_source(root) };
        const std::string zed { base::join_path(root, "zed") };
        (void) fs::create_directories(base::join_path(zed, "extensions/work/mcppls"));

        auto installed = editors::install_zed(zed, source);
        expect(installed.has_value()) << (installed ? "" : installed.error().message);
        expect(editors::zed_installed(zed));
        expect(fs::is_regular_file(base::join_path(zed, "extensions/installed/mcppls/extension.wasm")));

        // Installing again replaces what the first one left.
        expect(editors::install_zed(zed, source).has_value());

        expect(editors::remove_zed(zed));
        expect(!editors::zed_installed(zed));
        expect(!fs::exists(base::join_path(zed, "extensions/work/mcppls")));
        // Removing a link must not remove what it pointed at.
        expect(fs::is_regular_file(base::join_path(source, "extension.toml")));
        expect(!editors::remove_zed(zed));
        std::filesystem::remove_all(root);
    };

    "zed: an entry under the same name that is some other extension is refused"_test = [&] {
        const std::string root { scratch("zed-occupied") };
        const std::string source { zed_source(root) };
        const std::string zed { base::join_path(root, "zed") };
        const std::string other { base::join_path(zed, "extensions/installed/mcppls") };
        (void) fs::create_directories(other);
        (void) fs::write_file(base::join_path(other, "extension.toml"), "id = \"someone-else\"\n");
        auto installed = editors::install_zed(zed, source);
        expect(!installed.has_value());
        expect(fs::is_regular_file(base::join_path(other, "extension.toml")));
        std::filesystem::remove_all(root);
    };

    "zed: a source that was never built is refused"_test = [&] {
        const std::string root { scratch("zed-unbuilt") };
        const std::string source { base::join_path(root, "source") };
        (void) fs::create_directories(source);
        (void) fs::write_file(base::join_path(source, "extension.toml"), "id = \"mcppls\"\n");
        expect(!editors::install_zed(base::join_path(root, "zed"), source).has_value());
        std::filesystem::remove_all(root);
    };

    "clion: the archive unpacks into the plugins directory and is removed from it"_test = [&] {
        const std::string root { scratch("clion") };
        const std::string built { base::join_path(root, "built") };
        (void) fs::create_directories(base::join_path(built, "lib"));
        (void) fs::write_file(base::join_path(built, "lib/mcppls-clion.jar"), "jar");
        const std::string archive { base::join_path(root, "plugin.tar.gz") };
        const std::vector<mcppls::pack::archive::WriteEntry> entries { { built, "" } };
        expect(mcppls::pack::archive::write(archive, "mcppls-clion", entries).has_value());

        const std::string plugins { base::join_path(root, "CLion2025.2") };
        auto installed = editors::install_clion(plugins, archive);
        expect(installed.has_value()) << (installed ? "" : installed.error().message);
        expect(fs::is_regular_file(base::join_path(plugins, "mcppls-clion/lib/mcppls-clion.jar")));
        expect(editors::clion_installed(plugins));
        expect(editors::remove_clion(plugins));
        expect(!editors::clion_installed(plugins));
        std::filesystem::remove_all(root);
    };

    "clion: an archive that is not this plugin is refused"_test = [&] {
        const std::string root { scratch("clion-other") };
        const std::string built { base::join_path(root, "built") };
        (void) fs::create_directories(built);
        (void) fs::write_file(base::join_path(built, "a.jar"), "jar");
        const std::string archive { base::join_path(root, "other.tar.gz") };
        const std::vector<mcppls::pack::archive::WriteEntry> entries { { built, "" } };
        expect(mcppls::pack::archive::write(archive, "someone-else", entries).has_value());
        expect(!editors::install_clion(base::join_path(root, "plugins"), archive).has_value());
        std::filesystem::remove_all(root);
    };

    "the server directory is <user data>/mcppls/payload"_test = [&] {
        expect(editors::server_directory() == base::join_path(editors::user_data_directory(), "mcppls/payload"));
    };

    return report();
}
