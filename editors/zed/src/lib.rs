// The Zed side of mcpp-language-server: find the server and start it.
//
// Everything that makes mcppls worth using happens in the server itself, so this is small on
// purpose. The one decision it makes is where the server comes from: the worktree's own PATH,
// which is the environment the user's shell gives Zed — the same rule the server applies to the
// build tools it starts. That decision is resolve.rs, which `cargo test` covers on the host.
mod resolve;

use resolve::{resolve, Launch, Platform};
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
        let (os, _) = zed::current_platform();
        let platform = match os {
            zed::Os::Windows => Platform::Windows,
            zed::Os::Mac => Platform::Mac,
            zed::Os::Linux => Platform::Linux,
        };
        let Launch { command, args } = resolve(worktree.which("mcppls"), platform, &env)?;
        Ok(zed::Command {
            command,
            args,
            // The server finds its payload beside its own executable and reads the rest of what it
            // needs from the environment, so nothing else has to be passed here.
            env,
        })
    }
}

zed::register_extension!(Mcppls);
