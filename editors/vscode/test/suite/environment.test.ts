// Which editor a report came from (0.0.7 plan O-4). Code-OSS builds -- VSCodium, Code - OSS in termux,
// code-server -- differ from Microsoft's in ways a bug report needs to know, and the language client's
// `clientInfo` alone does not always say. The extension's own section of the diagnostic bundle and of the
// report carries the editor's appName, host and UI kind; this runs in every editor the suite runs in.
//
// MCPPLS_E2E_EXPECT_APP_NAME=<name> (set by the `code-oss-e2e` CI job to VSCodium) additionally pins
// the name, so a job that meant to run VSCodium and ran something else fails here instead of passing.

import * as assert from 'assert';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';

suite('the editor is named in the diagnostic environment', function () {
    this.timeout(120_000);

    let api: TestApi;

    suiteSetup(async () => {
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
    });

    test('the extension section names the running editor', function () {
        if (typeof api.environment !== 'function') {
            // A VSIX from before 0.0.7 has no such section to check.
            this.skip();
        }
        const section = api.environment();
        console.log(`extension section: ${JSON.stringify(section)}`);
        assert.strictEqual(section.appName, vscode.env.appName);
        assert.strictEqual(section.appHost, vscode.env.appHost);
        assert.strictEqual(section.uiKind, vscode.UIKind[vscode.env.uiKind]);
        assert.strictEqual(section.vscode, vscode.version);
        assert.ok(typeof section.appName === 'string' && section.appName.length > 0, 'appName is empty');
        const expected = process.env.MCPPLS_E2E_EXPECT_APP_NAME;
        if (expected) {
            assert.strictEqual(section.appName, expected);
        }
    });
});
