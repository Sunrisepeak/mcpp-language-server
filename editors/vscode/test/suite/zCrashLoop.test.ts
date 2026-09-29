// A server that keeps dying: the client restarts it twice, stops on the third crash, writes a crash
// report and offers the notification. Runs last (the file name sorts last) and leaves the server
// running again. The server is killed by its process id: it is a child of this extension host.
// POSIX only.

import * as assert from 'assert';
import { execFileSync } from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 300_000;

// The server's process id: the one child of this extension host that is not a zombie or one still
// shutting down. `undefined` while there are none or more than one (a stopped server takes a moment to exit).
function serverPid(): number | undefined {
    try {
        const out = execFileSync('pgrep', ['-P', String(process.pid), '-f', ' serve'], { encoding: 'utf8' });
        const pids = out.split('\n').map(Number).filter((pid) => Number.isFinite(pid) && pid > 0);
        return pids.length === 1 ? pids[0] : undefined;
    } catch {
        return undefined;
    }
}

async function until(what: string, condition: () => boolean, timeoutMs = 60_000): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!condition()) {
        if (Date.now() > deadline) throw new Error(`Timed out waiting for ${what}`);
        await new Promise((resolve) => setTimeout(resolve, 200));
    }
}

suite('server crash loop', function () {
    this.timeout(900_000);

    let api: TestApi;

    suiteSetup(async function () {
        if (process.platform === 'win32') this.skip();
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
    });

    test('a restart is not a crash; the third crash stops the restarts and offers the report', async () => {
        // Restarting twice by hand must not use up the budget.
        await vscode.commands.executeCommand('mcppls.restartServer');
        await vscode.commands.executeCommand('mcppls.restartServer');
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        assert.ok(!api.lastPrompt('unrecoverable')?.message.includes('stopped after crashing'), 'a restart by hand was counted as a crash');

        for (let crash = 1; crash <= 3; crash += 1) {
            let pid: number | undefined;
            await until('the server process', () => (pid = serverPid()) !== undefined);
            process.kill(pid as number, 'SIGKILL');
            if (crash < 3) {
                // Restarted by the client; wait until it is up again, so the next kill is a crash of a
                // running server and not of one still starting.
                await until('the server to come back', () => {
                    const again = serverPid();
                    return again !== undefined && again !== pid;
                });
                await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
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
