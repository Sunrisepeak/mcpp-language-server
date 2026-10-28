// Where the server comes from, and how it is started. This file does not name a Zed type, so it
// compiles for the machine `cargo test` runs on as well as for wasm32-wasip2.

/// The operating system the extension runs for; `lib.rs` maps Zed's own enum onto it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Platform {
    Linux,
    Mac,
    Windows,
}

/// What Zed is asked to run.
#[derive(Debug, PartialEq, Eq)]
pub struct Launch {
    pub command: String,
    pub args: Vec<String>,
}

pub const NOT_FOUND: &str = "mcppls is not on PATH. Build and install it from a checkout: \
                             mcpp run -p devtools -- extension --editor zed --install";

/// PATH first (`on_path` is what the worktree's `which` found): an `mcppls` the user installed -- the
/// universal package's launcher, a single file, or a build of their own. Then the payload
/// `mcpp run -p devtools -- extension --editor zed --install` puts at <user data>/mcppls/payload
/// (tools/devtools/src/editors.cppm), whose server is an image: the platform's runner `bin/okrun`
/// starts `bin/mcppls.okb`, as its payload.json's `server` says. The sandbox cannot look outside this
/// extension's work directory, so that layout is taken as written rather than read, and not checked to
/// exist: when it is missing, starting it fails with its path in the message. Only when neither can be
/// named is the error the one above.
pub fn resolve(on_path: Option<String>, platform: Platform, env: &[(String, String)]) -> Result<Launch, String> {
    if let Some(command) = on_path {
        return Ok(Launch { command, args: vec!["serve".to_string()] });
    }
    let payload = installed_payload(platform, env).ok_or_else(|| NOT_FOUND.to_string())?;
    let (separator, exe) = if platform == Platform::Windows { ('\\', ".exe") } else { ('/', "") };
    Ok(Launch {
        command: format!("{payload}{separator}bin{separator}okrun{exe}"),
        args: vec![format!("{payload}{separator}bin{separator}mcppls.okb"), "serve".to_string()],
    })
}

// <user data>/mcppls/payload, the same user data directory the installer uses: $XDG_DATA_HOME or
// ~/.local/share on Linux, ~/Library/Application Support on macOS, %LOCALAPPDATA% on Windows.
pub fn installed_payload(platform: Platform, env: &[(String, String)]) -> Option<String> {
    let var = |name: &str| {
        env.iter()
            .find(|(key, value)| key.eq_ignore_ascii_case(name) && !value.is_empty())
            .map(|(_, value)| value.clone())
    };
    match platform {
        Platform::Windows => var("LOCALAPPDATA").map(|data| format!("{data}\\mcppls\\payload")),
        Platform::Mac => var("HOME").map(|home| format!("{home}/Library/Application Support/mcppls/payload")),
        Platform::Linux => var("XDG_DATA_HOME")
            .filter(|data| data.starts_with('/'))
            .or_else(|| var("HOME").map(|home| format!("{home}/.local/share")))
            .map(|data| format!("{data}/mcppls/payload")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn env(pairs: &[(&str, &str)]) -> Vec<(String, String)> {
        pairs.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()
    }

    #[test]
    fn path_wins_over_the_installed_payload() {
        let launch = resolve(Some("/usr/bin/mcppls".into()), Platform::Linux, &env(&[("HOME", "/home/u")])).unwrap();
        assert_eq!(launch, Launch { command: "/usr/bin/mcppls".into(), args: vec!["serve".into()] });
    }

    #[test]
    fn the_installed_payload_runs_its_image_through_its_runner() {
        let launch = resolve(None, Platform::Linux, &env(&[("HOME", "/home/u")])).unwrap();
        assert_eq!(launch.command, "/home/u/.local/share/mcppls/payload/bin/okrun");
        assert_eq!(launch.args, ["/home/u/.local/share/mcppls/payload/bin/mcppls.okb", "serve"]);
    }

    #[test]
    fn xdg_data_home_moves_the_payload_when_absolute() {
        let e = env(&[("HOME", "/home/u"), ("XDG_DATA_HOME", "/data")]);
        assert_eq!(installed_payload(Platform::Linux, &e).unwrap(), "/data/mcppls/payload");
        let relative = env(&[("HOME", "/home/u"), ("XDG_DATA_HOME", "data")]);
        assert_eq!(installed_payload(Platform::Linux, &relative).unwrap(), "/home/u/.local/share/mcppls/payload");
        let empty = env(&[("HOME", "/home/u"), ("XDG_DATA_HOME", "")]);
        assert_eq!(installed_payload(Platform::Linux, &empty).unwrap(), "/home/u/.local/share/mcppls/payload");
    }

    #[test]
    fn neither_gives_the_error_that_names_the_fix() {
        let error = resolve(None, Platform::Linux, &[]).unwrap_err();
        assert_eq!(error, NOT_FOUND);
        assert!(error.contains("mcppls is not on PATH"));
        assert!(error.contains("extension --editor zed --install"));
        assert!(resolve(None, Platform::Windows, &env(&[("HOME", "C:\\Users\\u")])).is_err());
    }

    #[test]
    fn windows_runner_is_an_exe_under_local_app_data() {
        let launch = resolve(None, Platform::Windows, &env(&[("LocalAppData", "C:\\Users\\u\\AppData\\Local")])).unwrap();
        assert_eq!(launch.command, "C:\\Users\\u\\AppData\\Local\\mcppls\\payload\\bin\\okrun.exe");
        assert_eq!(launch.args, ["C:\\Users\\u\\AppData\\Local\\mcppls\\payload\\bin\\mcppls.okb", "serve"]);
    }

    #[test]
    fn mac_payload_is_under_application_support() {
        let launch = resolve(None, Platform::Mac, &env(&[("HOME", "/Users/u")])).unwrap();
        assert_eq!(launch.command, "/Users/u/Library/Application Support/mcppls/payload/bin/okrun");
        assert_eq!(launch.args[0], "/Users/u/Library/Application Support/mcppls/payload/bin/mcppls.okb");
    }
}
