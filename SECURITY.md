# Security

mcppls runs your build tool, spawns compilers, and ships a bundled clangd — treat it accordingly.

## Reporting a vulnerability

Please report security issues through GitHub's private security advisories on this repository:
open the **Security** tab → **Report a vulnerability**. Do not open a public issue for a
vulnerability. There is no separate mailing address to report to — use the advisory form so the
report stays private until a fix is ready.

## Trust boundaries

- **An untrusted VS Code workspace runs no build tool and no compiler.** Restricted Mode is
  honored: `vscode.workspace.isTrusted` gates the `--untrusted` flag passed to the server, which
  then refuses to run mcpp, CMake, or any discovery command (`src/project/mcpp.cpp`,
  `src/project/cmake.cpp`, `src/orchestrator/workspace.cpp`). Module navigation and the bundled
  semantic kit still work; the status area reports the reason (`untrusted-workspace`).
- **Build tool runs the server starts itself are offline by default.** `mcppls.buildTool`
  defaults to `offline`: mcpp/CMake are run without network access unless you set it to `online`,
  or `off` to never run them at all. This is enforced per run, not just at startup.
- **The bundled payload is checked at startup.** `mcppls-devtools payload` records the size and
  sha256 of every file it ships in `payload.json`; the server verifies them
  (`verify_payload_integrity`, `src/engine/payload.cppm`) before using clangd or the kit, and
  reports a mismatch instead of silently running a tampered or truncated binary.
- **AI/model features are off by default and make no network calls from the server process.**
  `mcppls.ai.enabled` defaults to `false`. When you turn it on and configure a model source, the
  server never talks to the network itself — it is built on openkal, which deliberately has no
  DNS resolution and no TLS (`tools/model-gateway/README.md`). Instead it starts a separate
  `mcppls-model` child process, which is the only thing that ever reaches a model endpoint, and
  only for the endpoint you configured. The editor's **Review Changes** command passes no model
  flag at all (`src/orchestrator/workspace.cpp`), so it runs the deterministic rules over git and
  the compiler locally and sends nothing anywhere.

## Supply chain

The payload bundles a fixed, hash-pinned clangd and libc++ build (`packaging/payload.lock.json`);
see [`NOTICE`](NOTICE) for what is bundled and under what license. Nothing is downloaded or
installed by the extension at run time.

## Scope

This covers the language server, the VS Code extension, and the packaging that produces the
payload. Vulnerabilities in mcpp, clangd/LLVM, or VS Code itself belong to those projects.
