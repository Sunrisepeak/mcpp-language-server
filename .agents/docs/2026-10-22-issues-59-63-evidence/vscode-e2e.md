# Local VS Code E2E evidence

## Persistent release-profile checkpoint after the environment rebuild

The actual VS Code 1.143.0 application completed the main suite with **44 passing, 1 expected
platform-specific pending**, exit 0, and workspace unchanged. It used the release-profile server
SHA-256 `a2b4386ec230a08d47afbd1ad4b0c0b7e7fce1efe16e9bb8b15b9dfb48fc0356`, built with
the target-only configuration isolation and polling-worker shutdown fixes. The later clang-cl
probe follow-up is not part of this binary. The payload was the hash-checked official v0.0.16
payload at `.payload-cache/59-63-release16/payload`, with engine `23.1.0-mcppls.4`.
The unfiltered development-extension run is retained in the persistent ignored log
`.payload-cache/59-63-vscode-final-source.log`. The extension and server both reported 0.0.17.
This is not the final published VSIX or upgrade test; those require the actual release assets.

## Earlier development-profile checkpoint

Date: 2026-10-22 JST. Host: Linux x86_64. CPU affinity: 24–27.

The actual VS Code stable 1.143.0 Electron application ran in an independent Xvfb display (`:91`).
The development extension came from this checkout. Its server was selected explicitly as:

```
/home/speak/workspace/github/mcpp-language-server/target/x86_64-linux-gnu/7a9b5869555e0029/bin/mcppls
```

Server SHA-256: `2da36dd18d44512affabffb635e43af45a2002bdd188e23aaa90f0aea7924ecd`.
The payload was explicitly `/tmp/mcppls-perf59/payload`, with maintained clangd `23.1.0-mcppls.4`;
the old extension-local payload was not used. Payload manifest SHA-256:
`eb3e0486d8e90cda67345f3a23fa5201f1e2e77d9073ac15ec6241bf8f8f6e02`.

| Run | Result | Evidence |
|---|---|---|
| `MCPPLS_E2E_ONLY=workspaceSwitch.test.js npm test` | 3 passing, exit 0, workspace unchanged | `/tmp/mcppls-59-63-vscode-workspace-e2e.log` |
| Main `npm test`, no file filter | 44 passing, 1 expected pending, exit 0, workspace unchanged | `/tmp/mcppls-59-63-vscode-main-e2e.log` |

Both runs include the new target-only workspace setting test: the running server remains available
and retains the same `about.startedAt` session identity after the setting changes. The focused test
took 3.246 seconds; the same test in the full main suite took 3.313 seconds. The single pending test
is the macOS Command Line Tools prompt test, which requires `MCPPLS_E2E_EXPECT_SDK_MISSING=1` in
its dedicated macOS clean-machine environment.

The host's shared inotify quota was exhausted, so Electron emitted `EMFILE` watcher warnings.
The full suite nevertheless passed its actual navigation, completion, editing, cache-reset,
configuration, restart, crash-recovery and workspace-integrity assertions. This run does not prove
native Windows/macOS behavior; those remain covered by the CI platform matrix.

No system packages or security settings were changed. Because passwordless sudo was unavailable,
Xvfb was downloaded with `apt-get download` and extracted under `/tmp/mcppls-vscode-e2e-deps`.
GTK/NSS/GBM/X11 runtime libraries were already installed. VS Code was downloaded through the
existing `@vscode/test-electron` helper into the ignored extension test cache.
