// A server that keeps dying: the client restarts it twice, stops on the third crash, writes a crash
// report and offers the notification. Runs last (the file name sorts last) and leaves the server
// running again. The server is killed by its process id: it is a child of this extension host.
// POSIX only.

import * as assert from 'assert';
import { execFileSync } from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 300_000;

// The server's process id: the one child of this extension host that is not a zombie or one still
// shutting down. `undefined` while there are none or more than one (a stopped server takes a moment to exit).
function serverPid(excluding?: number): number | undefined {
    try {
        const out = execFileSync('pgrep', ['-P', String(process.pid), '-f', ' serve'], { encoding: 'utf8' });
        const pids = out.split('\n').map(Number).filter((pid) => Number.isFinite(pid) && pid > 0 && pid !== excluding);
        return pids.length === 1 ? pids[0] : undefined;
    } catch {
        return undefined;
    }
}

async function until(what: string, condition: () => boolean, timeoutMs = 60_000, intervalMs = 200): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!condition()) {
        if (Date.now() > deadline) throw new Error(`Timed out waiting for ${what}`);
        await new Promise((resolve) => setTimeout(resolve, intervalMs));
    }
}

suite('server crash loop', function () {
    this.timeout(900_000);

    let api: TestApi;
    const unhandled: unknown[] = [];
    const onUnhandled = (reason: unknown): void => { unhandled.push(reason); };

    suiteSetup(async function () {
        if (process.platform === 'win32') this.skip();
        process.on('unhandledRejection', onUnhandled);
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        await until('the language client to finish starting', () => api.serverRunning());
    });

    suiteTeardown(() => {
        process.removeListener('unhandledRejection', onUnhandled);
        assert.deepStrictEqual(unhandled, [], 'crash recovery left unhandled promise rejections');
    });

    // The server answers initialize in milliseconds, too soon to be caught reliably between its spawn
    // and the client's start by polling the process table (a slow host, PRoot, lost that race).
    // MCPPLS_SERVER, read at every start, names a stand-in that waits before it becomes the real
    // server: the process exists with initialize unanswered for as long as the test needs.
    function delayedServer(): () => void {
        const extensionPath = vscode.extensions.getExtension(EXTENSION_ID)!.extensionPath;
        const payload = process.env.MCPPLS_PAYLOAD ? path.resolve(process.env.MCPPLS_PAYLOAD) : path.join(extensionPath, 'payload');
        const saved = process.env.MCPPLS_SERVER;
        const real = saved && saved.length > 0 ? saved : path.join(payload, 'bin', 'mcppls');
        const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-delayed-server-'));
        const standIn = path.join(directory, 'mcppls');
        fs.writeFileSync(standIn, `#!/bin/sh\nsleep 3\nexec '${real.replace(/'/g, `'\\''`)}' "$@"\n`, { mode: 0o755 });
        process.env.MCPPLS_SERVER = standIn;
        return () => {
            if (saved === undefined) delete process.env.MCPPLS_SERVER;
            else process.env.MCPPLS_SERVER = saved;
            fs.rmSync(directory, { recursive: true, force: true });
        };
    }

    test('a crash during initialization recovers with a fresh client', async () => {
        const previous = serverPid();
        const restore = delayedServer();
        try {
            const restarting = vscode.commands.executeCommand('mcppls.restartServer');
            let starting: number | undefined;
            await until('a replacement server still initializing', () => {
                starting = serverPid(previous);
                return starting !== undefined && starting !== previous && !api.serverRunning();
            }, 60_000, 50);
            const conflictPrompts = api.promptShownCount('conflict');
            process.kill(starting as number, 'SIGKILL');
            await restarting;
            // The previous, manually stopped server can still be cleaning up its engine.
            // Exclude it: its surviving PID is not evidence that the killed client recovered.
            await until('a new server with a fully initialized recovered client', () => {
                const recovered = serverPid(previous);
                return recovered !== undefined && recovered !== starting && api.serverRunning();
            });
            await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
            // Both command registration and a request through the new connection must work.
            assert.ok((await vscode.commands.getCommands(true)).includes('clangd.applyFix'));
            assert.ok(await api.cacheDetail(), 'the recovered client cannot request the cache report');
            assert.strictEqual(api.promptShownCount('conflict'), conflictPrompts, 'automatic recovery repeated the conflict prompt');
        } finally {
            restore();
        }
        await vscode.commands.executeCommand('mcppls.restartServer');
    });

    test('a restart is not a crash; the third crash stops the restarts and offers the report', async () => {
        // Restarting twice by hand must not use up the budget.
        await vscode.commands.executeCommand('mcppls.restartServer');
        await vscode.commands.executeCommand('mcppls.restartServer');
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        assert.ok(!api.lastPrompt('unrecoverable')?.message.includes('stopped after crashing'), 'a restart by hand was counted as a crash');

        for (let crash = 1; crash <= 3; crash += 1) {
            let pid: number | undefined;
            await until('the running server process', () => api.serverRunning() && (pid = serverPid()) !== undefined);
            process.kill(pid as number, 'SIGKILL');
            if (crash < 3) {
                // Restarted by the client; wait until it is up again, so the next kill is a crash of a
                // running server and not of one still starting.
                await until('the server to come back', () => {
                    const again = serverPid();
                    return again !== undefined && again !== pid;
                });
                await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
                await until('the restarted language client to finish starting', () => api.serverRunning());
            }
        }
        await until('the crash notification', () => api.lastPrompt('unrecoverable')?.message.includes('stopped after crashing 3 times') === true);
        const shown = api.lastPrompt('unrecoverable');
        assert.deepStrictEqual(shown?.items, ['Report Issue…', 'Restart Server', 'Turn Off in This Workspace', 'Show Logs']);
        assert.ok(shown?.message.includes('crash report'), shown?.message);
        const report = api.lastCrashReport();
        assert.ok(report && fs.existsSync(path.join(report, 'info.json')), `no crash report: ${String(report)}`);
        assert.ok(fs.existsSync(path.join(report, 'client.log')));
        assert.ok(fs.existsSync(path.join(report, 'server-log-tail.txt')), 'the server log tail is missing');
        await until('no restart', () => !api.serverRunning());

        // Restart Server brings it back.
        await vscode.commands.executeCommand('mcppls.restartServer');
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        assert.strictEqual(api.serverRunning(), true);
    });
});
