// The Zed side of mcpp-language-server: find the server and start it.
//
// Everything that makes mcppls worth using happens in the server itself, so this is small on
// purpose. The one decision it makes is where the server comes from (resolve.rs, which `cargo test`
// covers on the host): the command the user's settings name, else an `mcppls` on the worktree's PATH,
// else the payload of this extension's series -- the server, its clangd and its semantic kit --
// downloaded from the series' release into the work directory Zed gives this extension.
mod resolve;

use std::fs;

use resolve::{choose, initialization_options, Arch, Launch, Platform, Source};
use zed_extension_api::{self as zed, settings::LspSettings, LanguageServerId, LanguageServerInstallationStatus, Result};

struct Mcppls;

impl Mcppls {
    fn downloaded_server(&self, id: &LanguageServerId) -> Result<Launch> {
        let (os, arch) = zed::current_platform();
        let platform = match os {
            zed::Os::Windows => Platform::Windows,
            zed::Os::Mac => Platform::Mac,
            zed::Os::Linux => Platform::Linux,
        };
        let arch = match arch {
            zed::Architecture::Aarch64 => Arch::Aarch64,
            zed::Architecture::X8664 => Arch::X86_64,
            _ => Arch::Other,
        };
        let release = resolve::release();
        let payload = resolve::payload(&release.directory, platform, arch)?;
        let present = |path: &str| fs::metadata(path).is_ok_and(|m| m.is_file());
        if !(present(&payload.runner) && present(&payload.image)) {
            zed::set_language_server_installation_status(id, &LanguageServerInstallationStatus::Downloading);
            let fetched = (|| -> Result<()> {
                let found = zed::github_release_by_tag_name(resolve::REPOSITORY, &release.tag)?;
                let asset = found
                    .assets
                    .iter()
                    .find(|a| a.name == release.asset)
                    .ok_or_else(|| format!("the release has no {}", release.asset))?;
                // A directory an interrupted download left is not reused.
                fs::remove_dir_all(&release.directory).ok();
                zed::download_file(&asset.download_url, &release.directory, zed::DownloadedFileType::GzipTar)?;
                if platform != Platform::Windows {
                    zed::make_file_executable(&payload.runner)?;
                }
                if !(present(&payload.runner) && present(&payload.image)) {
                    return Err(format!("the archive holds no {} and {}", payload.runner, payload.image));
                }
                Ok(())
            })();
            if let Err(reason) = fetched {
                let message = resolve::download_failed(&release, &reason);
                zed::set_language_server_installation_status(id, &LanguageServerInstallationStatus::Failed(message.clone()));
                return Err(message);
            }
            // The payloads of earlier series are not needed again.
            if let Ok(entries) = fs::read_dir(".") {
                for entry in entries.flatten() {
                    if let Some(name) = entry.file_name().to_str() {
                        if resolve::is_stale(name, &release.directory) {
                            fs::remove_dir_all(entry.path()).ok();
                        }
                    }
                }
            }
        }
        zed::set_language_server_installation_status(id, &LanguageServerInstallationStatus::None);
        // The server runs in the worktree, not in the work directory, so it is given absolute paths.
        let work = std::env::current_dir().map_err(|e| format!("the extension's work directory: {e}"))?;
        let absolute = |relative: &str| work.join(relative).to_string_lossy().into_owned();
        Ok(Launch { command: absolute(&payload.runner), args: vec![absolute(&payload.image), "serve".to_string()] })
    }
}

impl zed::Extension for Mcppls {
    fn new() -> Self {
        Self
    }

    fn language_server_command(&mut self, id: &LanguageServerId, worktree: &zed::Worktree) -> Result<zed::Command> {
        let mut env = worktree.shell_env();
        let binary = LspSettings::for_worktree("mcppls", worktree).ok().and_then(|s| s.binary);
        if let Some(extra) = binary.as_ref().and_then(|b| b.env.clone()) {
            env.extend(extra);
        }
        let setting = binary.and_then(|b| b.path.map(|path| (path, b.arguments)));
        let Launch { command, args } = match choose(setting, worktree.which("mcppls")) {
            Source::Command(launch) => launch,
            Source::Release => self.downloaded_server(id)?,
        };
        // The server finds its payload around its own image (or executable) and reads the rest of
        // what it needs from the environment, so nothing else has to be passed here.
        Ok(zed::Command { command, args, env })
    }

    fn language_server_initialization_options(
        &mut self,
        _id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<Option<zed::serde_json::Value>> {
        let user = LspSettings::for_worktree("mcppls", worktree)
            .ok()
            .and_then(|settings| settings.initialization_options);
        Ok(Some(initialization_options(user)))
    }
}

zed::register_extension!(Mcppls);
