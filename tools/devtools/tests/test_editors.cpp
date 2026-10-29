// Installing into and removing from Zed and CLion, against scratch directories standing in for
// their data directories (mcppls.devtools.editors).
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.archive;
import mcppls.devtools.editors;
import mcpplibs.os;

namespace os = mcpplibs::os;

namespace editors = mcppls::devtools::editors;
namespace base = mcppls::base;

namespace {

std::string scratch(std::string_view name) {
    std::error_code ignored;
    const std::string directory { base::join_path(
        base::spelled(os::dirs::temp()),
        std::format("mcppls-test-editors-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)std::filesystem::create_directories(directory, ignored);
    return directory;
}

// An extension directory as the build leaves it.
std::string zed_source(const std::string& root, std::string_view id = "mcppls") {
    std::error_code ignored;
    const std::string source { base::join_path(root, "source") };
    (void)std::filesystem::create_directories(source, ignored);
    (void) os::fs::write(base::join_path(source, "extension.toml"), std::format("id = \"{}\"\nname = \"x\"\n", id));
    (void) os::fs::write(base::join_path(source, "extension.wasm"), "\0asm");
    return source;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "zed: install leaves the extension under installed/, remove takes it and its work directory"_test = [&] {
        std::error_code ignored;
        const std::string root { scratch("zed") };
        const std::string source { zed_source(root) };
        const std::string zed { base::join_path(root, "zed") };
        (void)std::filesystem::create_directories(base::join_path(zed, "extensions/work/mcppls"), ignored);

        auto installed = editors::install_zed(zed, source);
        expect(installed.has_value()) << (installed ? "" : installed.error().message);
        expect(editors::zed_installed(zed));
        expect(std::filesystem::is_regular_file(base::join_path(zed, "extensions/installed/mcppls/extension.wasm"), ignored));

        // Installing again replaces what the first one left.
        expect(editors::install_zed(zed, source).has_value());

        expect(editors::remove_zed(zed));
        expect(!editors::zed_installed(zed));
        expect(!std::filesystem::exists(base::join_path(zed, "extensions/work/mcppls"), ignored));
        // Removing a link must not remove what it pointed at.
        expect(std::filesystem::is_regular_file(base::join_path(source, "extension.toml"), ignored));
        expect(!editors::remove_zed(zed));
        std::filesystem::remove_all(root);
    };

    "zed: an entry under the same name that is some other extension is refused"_test = [&] {
        std::error_code ignored;
        const std::string root { scratch("zed-occupied") };
        const std::string source { zed_source(root) };
        const std::string zed { base::join_path(root, "zed") };
        const std::string other { base::join_path(zed, "extensions/installed/mcppls") };
        (void)std::filesystem::create_directories(other, ignored);
        (void) os::fs::write(base::join_path(other, "extension.toml"), "id = \"someone-else\"\n");
        auto installed = editors::install_zed(zed, source);
        expect(!installed.has_value());
        expect(std::filesystem::is_regular_file(base::join_path(other, "extension.toml"), ignored));
        std::filesystem::remove_all(root);
    };

    "zed: a source that was never built is refused"_test = [&] {
        std::error_code ignored;
        const std::string root { scratch("zed-unbuilt") };
        const std::string source { base::join_path(root, "source") };
        (void)std::filesystem::create_directories(source, ignored);
        (void) os::fs::write(base::join_path(source, "extension.toml"), "id = \"mcppls\"\n");
        expect(!editors::install_zed(base::join_path(root, "zed"), source).has_value());
        std::filesystem::remove_all(root);
    };

    "clion: the archive unpacks into the plugins directory and is removed from it"_test = [&] {
        std::error_code ignored;
        const std::string root { scratch("clion") };
        const std::string built { base::join_path(root, "built") };
        (void)std::filesystem::create_directories(base::join_path(built, "lib"), ignored);
        (void) os::fs::write(base::join_path(built, "lib/mcppls-clion.jar"), "jar");
        const std::string archive { base::join_path(root, "plugin.tar.gz") };
        const std::vector<mcppls::pack::archive::WriteEntry> entries { { built, "" } };
        expect(mcppls::pack::archive::write(archive, "mcppls-clion", entries).has_value());

        const std::string plugins { base::join_path(root, "CLion2025.2") };
        auto installed = editors::install_clion(plugins, archive);
        expect(installed.has_value()) << (installed ? "" : installed.error().message);
        expect(std::filesystem::is_regular_file(base::join_path(plugins, "mcppls-clion/lib/mcppls-clion.jar"), ignored));
        expect(editors::clion_installed(plugins));
        expect(editors::remove_clion(plugins));
        expect(!editors::clion_installed(plugins));
        auto carries = editors::clion_archive_carries_payload(archive);
        expect(carries.has_value() && !*carries) << "a build without a payload";
        std::filesystem::remove_all(root);
    };

    "clion: a system's package carries its payload, and installs with it"_test = [&] {
        std::error_code ignored;
        const std::string root { scratch("clion-package") };
        const std::string built { base::join_path(root, "built") };
        (void)std::filesystem::create_directories(base::join_path(built, "lib"), ignored);
        (void)std::filesystem::create_directories(base::join_path(built, "payload/bin"), ignored);
        (void) os::fs::write(base::join_path(built, "lib/mcppls-clion.jar"), "jar");
        (void) os::fs::write(base::join_path(built, "payload/payload.json"), "{}");
        (void) os::fs::write(base::join_path(built, "payload/bin/mcppls.okb"), "okb");
        const std::string archive { base::join_path(root, "plugin.tar.gz") };
        const std::vector<mcppls::pack::archive::WriteEntry> entries { { built, "" } };
        expect(mcppls::pack::archive::write(archive, "mcppls-clion", entries).has_value());

        auto carries = editors::clion_archive_carries_payload(archive);
        expect(carries.has_value() && *carries);
        const std::string plugins { base::join_path(root, "CLion2026.2") };
        expect(editors::install_clion(plugins, archive).has_value());
        expect(std::filesystem::is_regular_file(base::join_path(plugins, "mcppls-clion/payload/bin/mcppls.okb"), ignored));
        std::filesystem::remove_all(root);
    };

    "clion: an archive that is not this plugin is refused"_test = [&] {
        std::error_code ignored;
        const std::string root { scratch("clion-other") };
        const std::string built { base::join_path(root, "built") };
        (void)std::filesystem::create_directories(built, ignored);
        (void) os::fs::write(base::join_path(built, "a.jar"), "jar");
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
