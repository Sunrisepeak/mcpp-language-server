# VS Code client crash recovery evidence (0.0.12)

## Failure and scope

Product CI run `37658653689`, Intel macOS main VSIX suite, failed during the
server crash-loop test. Its log reports `Unexpected SIGPIPE`, duplicate command
`clangd.applyFix`, disposed pending responses, and destroyed-stream writes.
The downloaded authoritative log is `/tmp/joint-product-37658653689-failed.log`.
This record concerns the extension lifecycle; it does not establish the cause of
the macOS extension-host SIGPIPE or an engine crash.

The installed dependency is `vscode-languageclient` **10.1.1**, resolved in the
extension lockfile. Relevant shipped source is
`editors/vscode/node_modules/vscode-languageclient/lib/common/client.js`:

- `start()` (line 916) owns an internal `_onStart` promise separately from its
  async continuation, awaits initialize, then returns that promise.
- `doInitialize()` (line 1103) sets Running before awaiting the initialized
  notification and registering features. Its failure path calls `void stop()`.
- `handleConnectionClosed()` (line 1441) disposes the connection, awaits the
  custom closed handler, clears features and resets `_onStart`. Restart then
  starts the same client immediately.
- `lib/common/executeCommand.js` registers commands one at a time and stores
  their disposables only after the loop completes.

Resetting the start promise while its initialization continuation is still live
can orphan a rejected promise. Calling stop on a dead Starting client also
rejects. Reusing that client can overlap initialization and feature cleanup;
duplicate command registration in the macOS log is consistent with that race.
The overlapping-command explanation is an inference from source and the CI log,
not a Linux reproduction of the exact duplicate-command stack.

## Implemented lifecycle

The custom close handler waits for the original start promise to settle after
the library has disposed the transport, before the library clears features and
resets its promise. Automatic recovery returns DoNotRestart to the library and
queues disposal plus a fresh client in the host's existing lifecycle queue.
The three-crash budget is preserved. Automatic recovery skips `onStarting`,
preserving the existing behavior of not repeating conflict detection prompts.

`ServerLanguageClient.stop()` suppresses a failed shutdown only after that
client's transport has closed; other stop failures propagate. Together these
library guards are registered as **WA-VSCODE-003 /
[UP-V1](https://github.com/Sunrisepeak/mcpp-language-server/issues/24#issuecomment-6044639460)**.
Remove them only when
an updated upstream library handles initialization interruption without an
orphaned start promise or unhandled shutdown rejection, demonstrated by running
the initialization-crash test with the guards removed. Fresh-client recovery
can remain an extension lifecycle choice.

`runningClient()` becomes available only after `client.start()` completes,
including feature registration, rather than at the earlier Running transition.
The crash-loop test therefore kills fully initialized clients for its budget
checks and separately kills a replacement process during initialization.

## Linux proof

The real payload in `editors/vscode/payload` and installed VS Code 1.132.0 at
`/home/speak/.xlings/data/xpkgs/xim-x-code/1.132.0/code` were used with:

```sh
MCPPLS_E2E_EDITOR=/home/speak/.xlings/data/xpkgs/xim-x-code/1.132.0/code \
MCPPLS_E2E_ONLY=zCrashLoop.test.js npm test
```

The added initialization test polls for the newly spawned server PID, excluding
the previous server still shutting down, asserts the client is not initialized,
and sends SIGKILL. Recovery must register `clangd.applyFix` and answer a cache
request. The suite records unhandledRejection events and requires zero.

- Negative control: compile HEAD's original extension against the new tests.
  **1 passing, 2 failing**, about 5 seconds. The initialization test reports
  `failed to initialize: Pending response rejected since connection got disposed`;
  teardown captures two disposed-response rejections and two
  `Client is not running and can't be stopped ... starting` rejections.
  Log: `/tmp/mcppls-client-crash-baseline-red.log`.
- Fixed lifecycle: **2 passing**, about 6 seconds, zero captured unhandled
  rejections. Initialization interruption recovers; manual restarts do not use
  the crash budget; the third running-server crash stops recovery and creates
  the report; manual restart works afterward.
  Log: `/tmp/mcppls-client-crash-settled.log`.
- `npm run compile`, 115 unit tests and `git diff --check` passed.
- Final recovery-flag and conflict-prompt assertion run: **2 passing**, about
  6 seconds; no repeated conflict prompt and zero captured unhandled rejections.
  Log: `/tmp/mcppls-client-crash-final.log`.

An earlier exploratory attempt missed the replacement startup because PID
selection included both old and new children. It timed out and is not counted
as proof. The corrected PID selection produced the negative and positive
controls above. Another intermediate fresh-client-only version recovered but
still emitted unhandled rejections; it was not treated as a completed fix.

The fixed Linux run still logged a library message-queue write-after-end error
during deliberate SIGKILL. It was handled and did not appear in the suite's
unhandled-rejection collection. Temporary logs and compiled negative-control
files remain outside the PR; fixed source and compiled output were restored.

## Remaining verification

Intel macOS CI must verify the original VSIX scenario. Linux evidence does not prove macOS SIGPIPE
resolved, and no root-cause verdict is claimed for that signal. This change also
does not prove absence of all engine crashes or meet the complete joint release
gate by itself.
