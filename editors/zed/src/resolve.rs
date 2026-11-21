// Where the server comes from, and how it is started. This file does not name a Zed type, so it
// compiles for the machine `cargo test` runs on as well as for wasm32-wasip2.

/// The operating system the extension runs for; `lib.rs` maps Zed's own enum onto it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Platform {
    Linux,
    Mac,
    Windows,
}

/// The machine's architecture, as the payload names its runners' directories.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Arch {
    X86_64,
    Aarch64,
    /// One the payload has no runner for (32-bit x86).
    Other,
}

/// What Zed is asked to run.
#[derive(Debug, PartialEq, Eq)]
pub struct Launch {
    pub command: String,
    pub args: Vec<String>,
}

/// This extension's version, as extension.toml states it (`mcppls-devtools version --check` holds the
/// two together). The server serves a plugin of its own series only (docs/92-release.md "Versions").
pub const PLUGIN_VERSION: &str = "0.23.1";

/// The server whose payload the extension downloads: the newest server release of its series when the
/// extension was released. A server's patch reaches Zed with the extension's next patch, which names
/// it here, as it reaches VS Code and CLion inside their packages (docs/92-release.md "Versions").
pub const SERVER: &str = "0.23.0";

/// The repository whose releases carry the payload.
pub const REPOSITORY: &str = "Sunrisepeak/mcpp-language-server";

/// The initialization options the server receives: the user's (`lsp.mcppls.initialization_options`),
/// with `plugin` naming this extension and its version, so a server of another series refuses with
/// both versions instead of serving a plugin it was not built for.
pub fn initialization_options(user: Option<zed_extension_api::serde_json::Value>) -> zed_extension_api::serde_json::Value {
    use zed_extension_api::serde_json::{json, Value};
    let mut options = match user {
        Some(Value::Object(map)) => Value::Object(map),
        _ => json!({}),
    };
    options["plugin"] = json!({ "name": "zed", "version": PLUGIN_VERSION });
    options
}

/// Where the server comes from, in order: the command the user's settings name
/// (`lsp.mcppls.binary.path`; Zed itself starts that one with its `arguments` alone, and `serve` is the
/// default only where the extension is asked), an `mcppls` on the worktree's PATH
/// (the universal package's launcher, a single file, a build of one's own), and otherwise the payload
/// of this extension's series, which `lib.rs` downloads into the extension's work directory.
#[derive(Debug, PartialEq, Eq)]
pub enum Source {
    Command(Launch),
    Release,
}

pub fn choose(setting: Option<(String, Option<Vec<String>>)>, on_path: Option<String>) -> Source {
    if let Some((command, args)) = setting {
        return Source::Command(Launch { command, args: args.unwrap_or_else(|| vec!["serve".to_string()]) });
    }
    if let Some(command) = on_path {
        return Source::Command(Launch { command, args: vec!["serve".to_string()] });
    }
    Source::Release
}

/// `X.Y` of an `X.Y.Z`.
#[cfg(test)]
pub fn series(version: &str) -> &str {
    match version.rmatch_indices('.').next() {
        Some((at, _)) => &version[..at],
        None => version,
    }
}

/// The release that carries `SERVER`'s payload: `vX.Y.0` for a series' first server, `server-vX.Y.Z`
/// for a server's patch, which carries the payload (packaging/release.manifest.json).
#[derive(Debug, PartialEq, Eq)]
pub struct Release {
    pub tag: String,
    pub asset: String,
    /// The directory of the work directory the archive is unpacked into; it holds `payload/`.
    pub directory: String,
}

pub fn release() -> Release {
    release_of(SERVER)
}

pub fn release_of(server: &str) -> Release {
    let tag = if server.ends_with(".0") { format!("v{server}") } else { format!("server-v{server}") };
    Release {
        tag,
        asset: format!("mcppls-payload-{server}.tar.gz"),
        directory: format!("mcppls-payload-{server}"),
    }
}

/// The file written once a payload is unpacked and checked; a directory without it is downloaded
/// again (an unpacking cut short can leave a runner that exists but is not whole).
pub fn marker(release: &Release) -> String {
    format!("{}/.complete", release.directory)
}

/// The payload's server for this machine, relative to the work directory: this system's runner,
/// `payload/libexec/okrun/<system>-<arch>/okrun`, starting the server's image `payload/bin/mcppls.okb`.
/// The payload has a runner for linux-x86_64, linux-aarch64, macos-aarch64 and windows-x86_64.
#[derive(Debug, PartialEq, Eq)]
pub struct Payload {
    pub runner: String,
    pub image: String,
}

pub fn payload(directory: &str, platform: Platform, arch: Arch) -> Result<Payload, String> {
    let system = match (platform, arch) {
        (Platform::Linux, Arch::X86_64) => "linux-x86_64",
        (Platform::Linux, Arch::Aarch64) => "linux-aarch64",
        (Platform::Mac, Arch::Aarch64) => "macos-aarch64",
        (Platform::Windows, Arch::X86_64) => "windows-x86_64",
        _ => return Err(unsupported(platform, arch)),
    };
    let exe = if platform == Platform::Windows { ".exe" } else { "" };
    Ok(Payload {
        runner: format!("{directory}/payload/libexec/okrun/{system}/okrun{exe}"),
        image: format!("{directory}/payload/bin/mcppls.okb"),
    })
}

fn unsupported(platform: Platform, arch: Arch) -> String {
    format!(
        "mcppls's release has no server for {platform:?} {arch:?}: it has one for Linux x86_64 and aarch64, \
         macOS aarch64 and Windows x86_64. Put an mcppls you built on PATH, or name it in \
         lsp.mcppls.binary.path."
    )
}

/// What the user is told when the payload could not be downloaded or unpacked.
pub fn download_failed(release: &Release, reason: &str) -> String {
    format!(
        "mcppls {} could not be downloaded from github.com/{REPOSITORY} ({}): {reason}. Put mcppls on PATH, \
         or name it in lsp.mcppls.binary.path.",
        release.tag, release.asset
    )
}

/// A work directory's entry that an earlier series left: a payload directory other than `keep`.
pub fn is_stale(entry: &str, keep: &str) -> bool {
    entry.starts_with("mcppls-payload-") && entry != keep
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_options_name_this_plugin_beside_the_users_own() {
        use zed_extension_api::serde_json::json;
        let options = initialization_options(Some(json!({ "engine": { "name": "clangd" } })));
        assert_eq!(options["engine"]["name"], "clangd");
        assert_eq!(options["plugin"]["name"], "zed");
        assert_eq!(options["plugin"]["version"], PLUGIN_VERSION);
        assert_eq!(initialization_options(None)["plugin"]["version"], PLUGIN_VERSION);
    }

    #[test]
    fn the_constant_is_the_extensions_version() {
        let manifest = include_str!("../extension.toml");
        let line = manifest.lines().find(|l| l.starts_with("version = ")).unwrap();
        assert_eq!(line, format!("version = \"{PLUGIN_VERSION}\""));
    }

    #[test]
    fn the_settings_win_over_path_and_path_over_the_release() {
        let set = Some(("/opt/mcppls/bin/mcppls".to_string(), None));
        assert_eq!(
            choose(set, Some("/usr/bin/mcppls".into())),
            Source::Command(Launch { command: "/opt/mcppls/bin/mcppls".into(), args: vec!["serve".into()] })
        );
        let with_args = Some(("/p/okrun".to_string(), Some(vec!["/p/mcppls.okb".to_string(), "serve".to_string()])));
        assert_eq!(
            choose(with_args, None),
            Source::Command(Launch { command: "/p/okrun".into(), args: vec!["/p/mcppls.okb".into(), "serve".into()] })
        );
        assert_eq!(
            choose(None, Some("/usr/bin/mcppls".into())),
            Source::Command(Launch { command: "/usr/bin/mcppls".into(), args: vec!["serve".into()] })
        );
        assert_eq!(choose(None, None), Source::Release);
    }

    #[test]
    fn the_server_is_of_the_extensions_series() {
        assert_eq!(series("0.23.1"), "0.23");
        assert_eq!(series("1.4.0"), "1.4");
        assert_eq!(series(SERVER), series(PLUGIN_VERSION));
    }

    #[test]
    fn a_series_server_and_a_server_patch_have_their_own_tags() {
        let first = release_of("0.23.0");
        assert_eq!(first.tag, "v0.23.0");
        assert_eq!(first.asset, "mcppls-payload-0.23.0.tar.gz");
        assert_eq!(first.directory, "mcppls-payload-0.23.0");
        let patch = release_of("0.23.2");
        assert_eq!(patch.tag, "server-v0.23.2");
        assert_eq!(patch.asset, "mcppls-payload-0.23.2.tar.gz");
        assert_eq!(marker(&patch), "mcppls-payload-0.23.2/.complete");
        assert_eq!(release(), release_of(SERVER));
    }

    #[test]
    fn each_platform_takes_its_own_runner() {
        let p = payload("d", Platform::Linux, Arch::X86_64).unwrap();
        assert_eq!(p.runner, "d/payload/libexec/okrun/linux-x86_64/okrun");
        assert_eq!(p.image, "d/payload/bin/mcppls.okb");
        assert_eq!(payload("d", Platform::Linux, Arch::Aarch64).unwrap().runner, "d/payload/libexec/okrun/linux-aarch64/okrun");
        assert_eq!(payload("d", Platform::Mac, Arch::Aarch64).unwrap().runner, "d/payload/libexec/okrun/macos-aarch64/okrun");
        assert_eq!(payload("d", Platform::Windows, Arch::X86_64).unwrap().runner, "d/payload/libexec/okrun/windows-x86_64/okrun.exe");
    }

    #[test]
    fn a_platform_without_a_runner_names_the_ones_there_are() {
        for (platform, arch) in [(Platform::Mac, Arch::X86_64), (Platform::Windows, Arch::Aarch64), (Platform::Linux, Arch::Other)] {
            let error = payload("d", platform, arch).unwrap_err();
            assert!(error.contains("macOS aarch64"), "{error}");
            assert!(error.contains("lsp.mcppls.binary.path"), "{error}");
        }
    }

    #[test]
    fn a_failed_download_names_the_release_and_the_remedy() {
        let message = download_failed(&release(), "timed out");
        assert!(message.contains(&release().tag));
        assert!(message.contains(&release().asset));
        assert!(message.contains("timed out"));
        assert!(message.contains("PATH"));
    }

    #[test]
    fn only_another_servers_payload_is_stale() {
        assert!(is_stale("mcppls-payload-0.22.0", "mcppls-payload-0.23.0"));
        assert!(!is_stale("mcppls-payload-0.23.0", "mcppls-payload-0.23.0"));
        assert!(!is_stale("something-else", "mcppls-payload-0.23.0"));
    }
}
