// "Reset This Workspace's Cache" (0.0.7 plan C-1) against the real server: the command deletes the
// workspace's cache, the server plans again and the status is `ready` again. Skipped, with the reason,
// when the server does not list `mcppls.resetCache` (the released 0.0.6 server does not).

import * as assert from 'assert';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 300_000;

suite('reset this workspace\'s cache', function () {
    this.timeout(900_000);

    let api: TestApi;

    suiteSetup(async () => {
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
        const folder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(folder, 'the fixture workspace is not open');
        const document = await vscode.workspace.openTextDocument(vscode.Uri.joinPath(folder.uri, 'src', 'main.cpp'));
        await vscode.window.showTextDocument(document);
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
    });

    test('the command resets the cache and the server is ready again', async function () {
        const commands = typeof api.serverCommands === 'function' ? api.serverCommands() : [];
        if (!commands.includes('mcppls.resetCache')) {
            console.log(`skipped: the server does not list mcppls.resetCache (it lists: ${commands.join(', ') || 'nothing'})`);
            this.skip();
        }
        const answer = await vscode.commands.executeCommand<{ ok?: boolean; freedBytes?: number } | undefined>('mcppls.resetWorkspaceCache');
        console.log(`reset answer: ${JSON.stringify(answer)}`);
        assert.ok(answer, 'the command returned no answer from the server');
        assert.strictEqual(answer.ok, true);
        assert.ok(typeof answer.freedBytes === 'number' && answer.freedBytes >= 0, 'freedBytes is missing');
        const status = await api.waitForState('ready', READY_TIMEOUT_MS);
        assert.strictEqual(status.state, 'ready');
    });
});
