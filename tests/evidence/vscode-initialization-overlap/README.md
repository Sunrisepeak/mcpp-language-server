# Initialization recovery requires a new process

Native head21a3675 fails the Linux installed-editor initialization SIGKILL
case: its cache request sees no recovered client. The previous core receives
exit at01:58:06.607; the killed initializing server exits at06.677; replacement
startup begins06.693; the assertion fails06.695, before that replacement's
project setup. A normal process exit is only reported at07.301. These events
are consistent with the previous server still cleaning up during recovery.
Exact PIDs were not retained in the native log, so they are not invented here.

The test's initial PID lookup excludes the previous manually stopped process;
its recovery lookup previously did not. A previous surviving PID can therefore
satisfy recovered!=killed while the killed client's EOF has not settled. The
recovery lookup now excludes the previous PID too. All existing assertions
remain: a distinct new PID, initialized language client, ready/degraded status,
clangd.applyFix registration, successful actual cache request and no repeated
conflict prompt. No sleep, budget extension or retry of a failed cache request
was added.

An actual installed local VSIX run passes both initialization recovery and
three-crash/report recovery cases in6s, normal editor exit and unchanged project.
It uses current local product bytes with an existing payload and VS Code1.132.0;
this is focused Linux verification, not final maintained-package qualification.
Native re-run is required. Compact timeline, scope and raw hashes: result.json.
