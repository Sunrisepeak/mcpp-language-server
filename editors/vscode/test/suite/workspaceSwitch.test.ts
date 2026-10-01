// The per-workspace off switch (`mcppls.enable`) against the real server. It runs last (file names are
// sorted) because it stops the server and starts it again. The setting is written to the test
// workspace's .vscode/settings.json, which is removed again: the harness checks that the workspace is
// unchanged when the run ends.

import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 300_000;

async function until(what: string, condition: () => boolean, timeoutMs = 30_000): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    while (!condition()) {
        if (Date.now() > deadline) throw new Error(`Timed out waiting for ${what}`);
        await new Promise((resolve) => setTimeout(resolve, 100));
    }
}

suite('mcppls.enable switch', function () {
    this.timeout(900_000);

    let api: TestApi;

    suiteSetup(async () => {
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
    });

    suiteTeardown(async () => {
        await vscode.workspace.getConfiguration('mcppls').update('enable', undefined, vscode.ConfigurationTarget.Workspace);
        const workspace = process.env.MCPPLS_E2E_WORKSPACE;
        // Windows can still hold the directory while VS Code writes its settings (ENOTEMPTY, EPERM on CI): Node retries
        // those with maxRetries.
        if (workspace) fs.rmSync(path.join(workspace, '.vscode'), { recursive: true, force: true, maxRetries: 10, retryDelay: 200 });
    });

    test('false in the workspace stops the server and says so; true starts it again', async () => {
        assert.strictEqual(api.serverRunning(), true);
        await vscode.workspace.getConfiguration('mcppls').update('enable', false, vscode.ConfigurationTarget.Workspace);
        await until('the server to stop', () => !api.serverRunning());
        assert.strictEqual(api.serverEnabled(), false);
        await until('the status bar to say off', () => api.statusBarText().includes('C++ Modules: off in this workspace'));

        await vscode.workspace.getConfiguration('mcppls').update('enable', true, vscode.ConfigurationTarget.Workspace);
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        assert.strictEqual(api.serverRunning(), true);
        assert.ok(!api.statusBarText().includes('off in this workspace'), api.statusBarText());
    });

    test('the Turn Off command writes false and stops; the status item\'s command turns it back on', async () => {
        await vscode.commands.executeCommand('mcppls.turnOffInWorkspace');
        await until('the server to stop', () => !api.serverRunning());
        assert.strictEqual(vscode.workspace.getConfiguration('mcppls').inspect<boolean>('enable')?.workspaceValue, false);
        await until('the status bar to say off', () => api.statusBarText().includes('off in this workspace'));

        // A restart while it is off does not start anything.
        await vscode.commands.executeCommand('mcppls.restartServer');
        assert.strictEqual(api.serverRunning(), false);

        await vscode.commands.executeCommand('mcppls.turnOnInWorkspace');
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        assert.strictEqual(api.serverRunning(), true);
        assert.strictEqual(vscode.workspace.getConfiguration('mcppls').inspect<boolean>('enable')?.workspaceValue, undefined);
    });
});
