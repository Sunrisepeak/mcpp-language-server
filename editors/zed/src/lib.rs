// The Zed side of mcpp-language-server: find the server and start it.
//
// Everything that makes mcppls worth using happens in the server itself, so this is small on
// purpose. The one decision it makes is where the server comes from: the worktree's own PATH,
// which is the environment the user's shell gives Zed — the same rule the server applies to the
// build tools it starts.
use zed_extension_api::{self as zed, Result};

struct Mcppls;

impl zed::Extension for Mcppls {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let env = worktree.shell_env();
        // PATH first; then the server `mcpp run -p devtools -- extension --editor zed --install`
        // puts at <user data>/mcppls/payload (tools/devtools/src/editors.cppm). The sandbox cannot
        // look outside this extension's work directory, so that one is not checked here: when it
        // is missing too, starting it fails with its path in the message.
        let command = worktree
            .which("mcppls")
            .or_else(|| installed_server(&env))
            .ok_or_else(|| {
                "mcppls is not on PATH. Build and install it from a checkout: \
                 mcpp run -p devtools -- extension --editor zed --install"
                    .to_string()
            })?;
        Ok(zed::Command {
            command,
            args: vec!["serve".to_string()],
            // The server finds its payload beside its own executable and reads the rest of what it
            // needs from the environment, so nothing else has to be passed here.
            env,
        })
    }
}

// <user data>/mcppls/payload/bin/mcppls, with the same user data directory the installer uses:
// $XDG_DATA_HOME or ~/.local/share on Linux, ~/Library/Application Support on macOS, %LOCALAPPDATA%
// on Windows.
fn installed_server(env: &[(String, String)]) -> Option<String> {
    let var = |name: &str| {
        env.iter()
            .find(|(key, value)| key.eq_ignore_ascii_case(name) && !value.is_empty())
            .map(|(_, value)| value.clone())
    };
    let (os, _) = zed::current_platform();
    match os {
        zed::Os::Windows => var("LOCALAPPDATA").map(|data| format!("{data}\\mcppls\\payload\\bin\\mcppls.exe")),
        zed::Os::Mac => var("HOME").map(|home| format!("{home}/Library/Application Support/mcppls/payload/bin/mcppls")),
        zed::Os::Linux => var("XDG_DATA_HOME")
            .filter(|data| data.starts_with('/'))
            .or_else(|| var("HOME").map(|home| format!("{home}/.local/share")))
            .map(|data| format!("{data}/mcppls/payload/bin/mcppls")),
    }
}

zed::register_extension!(Mcppls);
