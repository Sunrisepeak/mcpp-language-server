// The "conflicts" scenario (plan W6.4, design 16.5, U10): with the cpptools
// and clangd stub extensions installed (test/stubs/*), mcppls must ask
// once whether to turn off their language features, act only on the answer
// runTest.ts substituted (via MCPPLS_E2E_CONFLICT_ANSWER, read below and
// handed to the extension through TestApi.setPromptAnswer before the
// extension's own automatic check can reach it -- see src/prompt.ts), and
// remember that answer afterwards. runTest.ts runs this suite twice, once
// per answer, each against its own fresh workspace and profile.
//
// Test order matters here, unusually for Mocha: "the question is asked
// exactly once" awaits conflictCheck() to full completion, which is also
// the only guarantee that any settings.json write it performs has finished;
// the later tests rely on running after it for that reason, not only for
// their own assertions.

import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { DISABLE, KEEP } from '../../src/conflicts';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 120_000;

function settingsFile(): string {
    const folder = vscode.workspace.workspaceFolders![0];
    return path.join(folder.uri.fsPath, '.vscode', 'settings.json');
}

function readWorkspaceSettings(): Record<string, unknown> {
    try {
        return JSON.parse(fs.readFileSync(settingsFile(), 'utf8')) as Record<string, unknown>;
    } catch (error) {
        if ((error as NodeJS.ErrnoException).code === 'ENOENT') {
            return {};
        }
        throw error;
    }
}

suite('conflicting C++ extensions', function () {
    this.timeout(180_000);

    let api: TestApi;
    const answer = process.env.MCPPLS_E2E_CONFLICT_ANSWER;

    suiteSetup(async function () {
        assert.ok(
            answer === DISABLE || answer === KEEP,
            `MCPPLS_E2E_CONFLICT_ANSWER must be "${DISABLE}" or "${KEEP}"; got ${JSON.stringify(answer)}`,
        );
        assert.ok(vscode.extensions.getExtension('ms-vscode.cpptools'), 'the cpptools stub is not installed');
        assert.ok(vscode.extensions.getExtension('llvm-vs-code-extensions.vscode-clangd'), 'the clangd stub is not installed');

        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
        api.setPromptAnswer('conflict', answer);

        const folder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(folder, 'the fixture workspace is not open');
        const document = await vscode.workspace.openTextDocument(vscode.Uri.joinPath(folder.uri, 'src', 'main.cpp'));
        await vscode.window.showTextDocument(document);
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
    });

    test('the question is asked exactly once', async () => {
        assert.strictEqual(await api.conflictCheck(), 'asked');
        assert.strictEqual(api.promptShownCount('conflict'), 1);
    });

    if (answer === DISABLE) {
        test('disabling writes exactly the two settings and nothing else', () => {
            assert.deepStrictEqual(readWorkspaceSettings(), {
                'C_Cpp.intelliSenseEngine': 'disabled',
                'clangd.enable': false,
            });
        });

        // design 2026-09-25 §10 "Check again when things change": re-enabling a setting this
        // extension turned off is a conflict becoming active again, and gets a notice -- distinct
        // from the one-time question above, and never a warning or error modal.
        test('re-enabling clangd.enable shows a non-modal notice, once', async () => {
            const before = api.notificationCount();
            await vscode.workspace.getConfiguration('clangd').update('enable', true, vscode.ConfigurationTarget.Workspace);
            await new Promise((resolve) => setTimeout(resolve, 3000));
            assert.ok(
                api.notificationCount() > before,
                `expected a new notice once clangd.enable was re-enabled; count stayed at ${api.notificationCount()}`,
            );
        });

        // design §10 "Commands": mcppls.turnOffOtherCppFeatures acts on whatever is active right
        // now, at the scope chosen by the quick pick (substituted here the same way as the
        // conflictAnswer prompt; see src/prompt.ts's pickOnce). cpptools is still off from the
        // first-run answer above, so only clangd (re-enabled by the previous test) is active.
        test('mcppls.turnOffOtherCppFeatures turns the newly re-enabled conflict back off, at workspace scope', async () => {
            api.setPromptAnswer('turnOffScope', 'workspace');
            await vscode.commands.executeCommand('mcppls.turnOffOtherCppFeatures');
            assert.deepStrictEqual(readWorkspaceSettings(), {
                'C_Cpp.intelliSenseEngine': 'disabled',
                'clangd.enable': false,
            });
        });

        // design §10 "Restore Other C++ Language Features": puts back exactly what the most
        // recent turn-off remembered -- here, `true` (what the test itself set clangd.enable to,
        // right before the command above disabled it again), not `false` (the very first
        // ask-once answer's value) and not cpptools's setting at all, which that command left
        // alone because cpptools was not active when it ran. cpptools never had an override
        // before the very first disable either, so restoring it removes the key entirely rather
        // than writing a value that happens to match today's default.
        test('mcppls.restoreOtherCppFeatures puts back the value from right before the last turn-off', async () => {
            await vscode.commands.executeCommand('mcppls.restoreOtherCppFeatures');
            assert.deepStrictEqual(readWorkspaceSettings(), { 'clangd.enable': true });
        });
    } else {
        test('keeping both changes no settings', () => {
            assert.deepStrictEqual(readWorkspaceSettings(), {});
        });
    }

    test('restarting the server does not ask again', async () => {
        await vscode.commands.executeCommand('mcppls.restartServer');
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        assert.strictEqual(api.promptShownCount('conflict'), 1, 'restarting the server must not ask again');
        assert.strictEqual(await api.conflictCheck(), 'already-answered');
    });
});
