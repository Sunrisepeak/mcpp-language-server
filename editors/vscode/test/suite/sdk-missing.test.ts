// Exercises the macOS "Command Line Tools" prompt (plan W5.4, U7): the
// server reports state 'degraded' with an sdk-missing issue when it cannot
// find the macOS SDK. This only genuinely happens on a clean-machine CI job
// that hides the Command Line Tools before running this suite; there is no
// way to simulate that locally short of actually uninstalling Xcode's
// Command Line Tools. So this file runs its assertions only when
// MCPPLS_E2E_EXPECT_SDK_MISSING=1, and skips itself cleanly everywhere
// else -- including every local run and the ordinary CI hosts, all of which
// have a real SDK and would otherwise never reach the 'degraded' state this
// test requires.

import * as assert from 'assert';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const STATUS_TIMEOUT_MS = 120_000;

suite('macOS Command Line Tools prompt', function () {
    this.timeout(180_000);

    suiteSetup(function () {
        if (process.env.MCPPLS_E2E_EXPECT_SDK_MISSING !== '1') {
            this.skip();
        }
    });

    test('asks once, and does not ask again after a restart', async () => {
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        const api = await extension.activate();
        // Dismiss without triggering the (test-mode-recorded, never really
        // spawned) install: this test is only about how often the question
        // is put, not about the install path itself.
        api.setPromptAnswer('commandLineTools', 'Not now');

        const status = await api.waitForState('degraded', STATUS_TIMEOUT_MS);
        assert.ok(
            (status.issues ?? []).some((issue) => issue.code === 'sdk-missing'),
            `expected a degraded status with an sdk-missing issue; got ${JSON.stringify(status)}`,
        );
        assert.strictEqual(api.promptShownCount('commandLineTools'), 1);

        await vscode.commands.executeCommand('mcppls.restartServer');
        await api.waitForState('degraded', STATUS_TIMEOUT_MS);
        assert.strictEqual(api.promptShownCount('commandLineTools'), 1, 'restarting the server must not ask again');
    });
});
