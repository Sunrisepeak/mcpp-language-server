// The cache UI (0.0.10 plan C-13) against the real server: the sweep command answers, the status
// bar stays ONE item and NO webview ever appears -- the hover card and the hub are native controls,
// and the E2E keeps them that way. Skipped when the server does not list `mcppls.sweepCache`.

import * as assert from 'assert';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 300_000;

suite('the cache hub and the status bar', function () {
    this.timeout(900_000);

    let api: TestApi;

    suiteSetup(async () => {
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
        const folder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(folder, 'the suite runs with the fixture workspace open');
        const document = await vscode.workspace.openTextDocument(vscode.Uri.joinPath(folder.uri, 'src', 'main.cpp'));
        await vscode.window.showTextDocument(document);
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
    });

    test('the status bar item stays one item with the cache as its segment, under its budget', () => {
        const text = api.statusBarText();
        assert.ok(text.includes('C++ Modules'), text);
        // One item: the cache may be appended, the module text is never replaced by it (D10/D12).
        const occurrences = text.split('C++ Modules').length - 1;
        assert.strictEqual(occurrences, 1);
        assert.ok(text.length <= 60, `the whole item stays short: ${text}`);
    });

    test('sweeping the cache answers ok and frees without restarting the engine (S3 5.8)', async function () {
        const commands = api.serverCommands();
        if (!commands.includes('mcppls.sweepCache')) {
            this.skip();
        }
        const before = await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        const answer = (await vscode.commands.executeCommand('mcppls.sweepWorkspaceCache')) as { ok?: boolean; freedBytes?: number };
        assert.strictEqual(answer.ok, true);
        const after = await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        assert.strictEqual(after.state, before.state, 'a sweep neither stops nor restarts the engine');
    });

    test('no webview: the card and the hub are native controls, locked by the counter', () => {
        assert.strictEqual(api.webviewPanelCount(), 0);
    });
});
