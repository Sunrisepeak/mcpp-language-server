// An unrecoverable status issue produces one non-modal notification with the right actions. Nothing is
// put on screen in test mode (src/prompt.ts): the notification is recorded, and the issues are handed to
// the same code the server's status goes through (TestApi.injectIssues), so this works with a server that
// does not send `bundle` yet.

import * as assert from 'assert';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';

suite('unrecoverable error notification', function () {
    this.timeout(60_000);

    let api: TestApi;

    suiteSetup(async () => {
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
    });

    test('an issue with a bundle shows the notification once, with its actions', () => {
        const issue = { code: 'payload-corrupt', message: 'The bundled clangd does not match its checksum.', bundle: '/tmp/mcppls-auto-bundle.zip' };
        assert.deepStrictEqual(api.injectIssues([issue]), ['payload-corrupt']);
        const shown = api.lastPrompt('unrecoverable');
        assert.ok(shown, 'no notification was shown');
        assert.ok(shown.message.includes(issue.message), shown.message);
        assert.ok(shown.message.includes('mcppls saved a diagnostic bundle (it stays on this machine).'), shown.message);
        assert.deepStrictEqual(shown.items, [
            'Report Issue…', 'Restart Server', 'Reset This Workspace\'s Cache', 'Turn Off in This Workspace', 'Show Logs']);
        assert.deepStrictEqual(api.injectIssues([issue]), [], 'the same code is told once per session');
    });

    test('a fatal code without a bundle (an older server) is told too; a recoverable one is not', () => {
        assert.deepStrictEqual(api.injectIssues([{ code: 'module-lock-stale', message: 'A lock nobody holds.' }]), []);
        assert.deepStrictEqual(api.injectIssues([{ code: 'engine-incompatible', message: 'clangd cannot load this module cache.' }]), ['engine-incompatible']);
        const shown = api.lastPrompt('unrecoverable');
        assert.ok(shown?.message.includes('Report Issue writes a diagnostic bundle first'), shown?.message);
    });
});
