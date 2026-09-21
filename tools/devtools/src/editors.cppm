// Where Zed, CLion and the server they start live on this machine, and the file operations that
// install and remove them. No building and no printing here: packaging.cpp decides what to build
// and says what happened; this module only knows the places and does the moves, so the tests can
// point it at a scratch directory.
//
// Zed and CLion both start `mcppls` from PATH first. A server installed here, the one the VS Code
// extension carries inside itself, is their fallback: `<user data>/mcppls/payload`, which both
// plugins compute the same way (editors/zed/src/lib.rs, editors/clion/.../McpplsServerSupportProvider.kt).
export module mcppls.devtools.editors;

import std;
import mcppls.base.error;

export namespace mcppls::devtools::editors {

// The per-user data directory the three of them build on: $XDG_DATA_HOME or ~/.local/share on
// Linux, ~/Library/Application Support on macOS, %LOCALAPPDATA% on Windows.
std::string user_data_directory();

// <user data>/mcppls/payload: the server Zed and CLion fall back to when PATH has none.
std::string server_directory();

// ---- Zed -------------------------------------------------------------------------------------
// A dev extension is what Zed's own "zed: install dev extension" leaves behind: a symbolic link
// `extensions/installed/<id>` to the source directory, found by the watcher on `installed/` and
// marked dev because it is a link (extension_host.rs, `add_extension_to_index`). Doing the same
// here is doing what the palette does, minus the compile, which has already happened.

inline constexpr std::string_view ZED_EXTENSION_ID { "mcppls" };

// Zed's data directory: ~/.local/share/zed, ~/Library/Application Support/Zed or %LOCALAPPDATA%\Zed.
std::string zed_directory();

enum class ZedInstall { linked, copied };

// Links (or, where no link can be made, copies extension.toml and extension.wasm) `source` into
// `<zedDirectory>/extensions/installed/<id>`. What is already there under that id is replaced when
// it is a link or a copy of this extension, and refused otherwise.
base::Result<ZedInstall> install_zed(const std::string& zedDirectory, const std::string& source);

// Removes the installed entry and the extension's work directory. False when nothing was there.
bool remove_zed(const std::string& zedDirectory);

bool zed_installed(const std::string& zedDirectory);

// ---- CLion -----------------------------------------------------------------------------------
// An IntelliJ plugin installed from disk is its archive unpacked into the IDE's plugins directory,
// which is what "Install Plugin from Disk" does too; the IDE loads it at its next start.

// The directory the plugin archive unpacks to: the Gradle project's name (settings.gradle.kts).
inline constexpr std::string_view CLION_PLUGIN_DIRECTORY { "mcppls-clion" };

// Every CLion's plugins directory on this machine, newest first: <JetBrains>/CLion<version>, plus
// `plugins/` on macOS and Windows. Empty when no CLion has run here.
std::vector<std::string> clion_plugin_directories();

// Unpacks `archive` into `pluginsDirectory`, replacing an earlier copy of this plugin.
base::Result<void> install_clion(const std::string& pluginsDirectory, const std::string& archive);

bool remove_clion(const std::string& pluginsDirectory);

bool clion_installed(const std::string& pluginsDirectory);

} // namespace mcppls::devtools::editors
