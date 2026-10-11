import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

suite('Installed VSIX formatting', function () {
    this.timeout(90_000);

    test('the editor formatting provider reproduces the complete pinned golden', async function () {
        const extension = vscode.extensions.getExtension<TestApi>('sunrisepeak.mcpp-language-server');
        assert.ok(extension, 'the packaged extension must be installed');
        const api = await extension.activate();
        const folder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(folder, 'the isolated fixture must be open');
        const document = await vscode.workspace.openTextDocument(vscode.Uri.joinPath(folder.uri, 'src', 'format.cppm'));
        await vscode.window.showTextDocument(document);
        await api.waitForState(['ready', 'degraded'], 60_000);
        const variant = process.env.MCPPLS_E2E_FORMAT_VARIANT;
        const golden = variant === 'user' ? 'google' : variant === 'project' ? 'llvm' : 'mcpp';
        const expected = fs.readFileSync(path.join(folder.uri.fsPath, `expected-${golden}.txt`), 'utf8');
        const original = document.getText();
        assert.notStrictEqual(original, expected, 'the golden must require actual formatting edits');
        const deadline = Date.now() + 60_000;
        let actual = original;
        while (Date.now() < deadline) {
            const edits = await vscode.commands.executeCommand<vscode.TextEdit[]>(
                'vscode.executeFormatDocumentProvider', document.uri, { tabSize: 4, insertSpaces: true });
            actual = original;
            const replacements = (edits ?? []).map((edit) => ({
                start: document.offsetAt(edit.range.start),
                end: document.offsetAt(edit.range.end),
                text: edit.newText,
            })).sort((left, right) => right.start - left.start);
            let previousStart = original.length;
            for (const edit of replacements) {
                assert.ok(edit.start <= edit.end && edit.end <= previousStart, 'format edits must not overlap');
                actual = actual.slice(0, edit.start) + edit.text + actual.slice(edit.end);
                previousStart = edit.start;
            }
            if (actual === expected) {
                break;
            }
            await new Promise((resolve) => setTimeout(resolve, 250));
        }
        assert.strictEqual(actual, expected);
        assert.strictEqual(document.getText(), original, 'checking edits must not modify the fixture');
        assert.strictEqual(document.isDirty, false);
    });
});
