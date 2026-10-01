// WA-VSCODE-002 (0.0.8 plan E-1, E-3, E-4): in C and C++ the completion list opens while you type, whatever
// VS Code's own default for `editor.quickSuggestions` is (since 1.125 it waits for inline completions such
// as Copilot's first). The suite runs in a fresh profile, so the extension's language default is what C++
// sees, other languages keep the editor's own, and the extension's part of the diagnostic bundle says both.

import * as assert from 'assert';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const OURS = { other: 'on', comments: 'off', strings: 'off' };

suite('WA-VSCODE-002: completion shows while typing in C and C++', function () {
    this.timeout(120_000);

    let api: TestApi;

    suiteSetup(async () => {
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
    });

    test('C and C++ get the extension\'s language default, other languages the editor\'s own', function () {
        for (const languageId of ['cpp', 'c']) {
            const editor = vscode.workspace.getConfiguration('editor', { languageId });
            const inspected = editor.inspect('quickSuggestions');
            console.log(`${languageId}: ${JSON.stringify(inspected)}`);
            assert.deepStrictEqual(inspected?.defaultLanguageValue, OURS, `${languageId}: the contributed default`);
            assert.deepStrictEqual(editor.get('quickSuggestions'), OURS, `${languageId}: what the editor uses`);
        }
        const python = vscode.workspace.getConfiguration('editor', { languageId: 'python' });
        assert.deepStrictEqual(python.get('quickSuggestions'), python.inspect('quickSuggestions')?.defaultValue,
            'a language the extension does not serve keeps the editor\'s own default');
    });

    test('the bundle\'s editor section names the value and where it comes from', function () {
        if (typeof api.editor !== 'function') {
            // A VSIX from before 0.0.8 has no such section.
            this.skip();
        }
        const section = api.editor() as { quickSuggestions?: { value?: unknown; source?: string }; inlineCompletionExtensions?: unknown };
        console.log(`editor section: ${JSON.stringify(section)}`);
        assert.deepStrictEqual(section.quickSuggestions?.value, OURS);
        assert.strictEqual(section.quickSuggestions?.source, 'defaultLanguage');
        assert.ok(Array.isArray(section.inlineCompletionExtensions), 'the inline-completion extensions are listed (empty here)');
    });
});
