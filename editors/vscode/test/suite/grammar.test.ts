// Layer 1 of highlighting `import` (design 2026-09-25 §7, WA-VSCODE-001): the injection grammar
// (syntaxes/mcppls-modules.tmLanguage.json) colors the module syntax VS Code's own cpp grammar
// misses, on every keystroke and with no server involved. This opens a scratch .cpp file (a real
// file on disk, outside the fixture workspace, because `_workbench.captureSyntaxTokens` needs a
// filesystem provider and cannot tokenize an untitled document) and reads back each line's scopes.
//
// See test/suite/workaroundCanary.test.ts for the other half: the check that VS Code's own grammar
// still needs this layer at all.

import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as vscode from 'vscode';

interface CapturedToken {
    c: string; // the token's text
    t: string; // its full scope stack, space-separated, outermost first
}

// captureSyntaxTokens returns one flat list for the whole file, with no token (not even an empty
// one) marking where a line ends -- token text simply runs from one line straight into the next.
// So a token is assigned to a line by walking the *known* line lengths and consuming that many
// characters of token text per line; nothing here ever crosses a line, since each pattern in the
// grammar only ever matches within one line.
function tokensByLine(sourceLines: readonly string[], tokens: readonly CapturedToken[]): { text: string; scopes: string[] }[][] {
    const lines: { text: string; scopes: string[] }[][] = sourceLines.map(() => []);
    let lineIndex = 0;
    let consumed = 0;
    for (const token of tokens) {
        while (lineIndex < sourceLines.length && consumed >= sourceLines[lineIndex].length) {
            lineIndex += 1;
            consumed = 0;
        }
        if (lineIndex >= sourceLines.length) {
            break;
        }
        lines[lineIndex].push({ text: token.c, scopes: token.t.split(' ') });
        consumed += token.c.length;
    }
    return lines;
}

function findToken(line: { text: string; scopes: string[] }[], text: string): { text: string; scopes: string[] } | undefined {
    return line.find((token) => token.text === text);
}

async function captureLines(lines: readonly string[]): Promise<{ text: string; scopes: string[] }[][]> {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-grammar-'));
    const file = path.join(dir, 'probe.cpp');
    fs.writeFileSync(file, lines.join('\n'));
    const uri = vscode.Uri.file(file);
    const document = await vscode.workspace.openTextDocument(uri);
    await vscode.window.showTextDocument(document);
    const captured = await vscode.commands.executeCommand<CapturedToken[]>('_workbench.captureSyntaxTokens', uri);
    // The probe is no part of the fixture's project: close it, so the suites after this one see only the project.
    await vscode.commands.executeCommand('workbench.action.closeActiveEditor');
    return tokensByLine(lines, captured);
}

suite('module-syntax highlighting: the injected grammar (WA-VSCODE-001)', function () {
    this.timeout(60_000);

    let lines: { text: string; scopes: string[] }[][];

    suiteSetup(async () => {
        lines = await captureLines([
            'module;',
            'export module a.b;',
            'module :private;',
            // The probe is opened in a real editor, so clangd sees it too, with the command it makes up for a file outside
            // the project from the nearest unit's. Under that command clangd 23.1 (and main at 510126255) never finishes a
            // file in which `import std;` comes before an `export import` of something nothing provides, and the session
            // would sit in `preparing` for two minutes: this `export import` comes first.
            'export import :part;',
            'import hello.greet;',
            'import hello.',
            'import std;',
            'import <vector>;',
            'import "foo.h";',
            'x = import;',
            'obj.import(1);',
            'int module = 5;',
            // Fix plan F8: the export of an export declaration.
            'export namespace ns {',
            '  export int inner();',
            '}',
            'export {',
            '}',
            'export template <class T> T g(T);',
            'exports = 1;',
        ]);
    });

    test('the export of an export declaration is colored like export module\'s (F8)', () => {
        for (const index of [12, 13, 15, 17]) {
            const token = findToken(lines[index], 'export');
            assert.ok(token?.scopes.includes('keyword.control.export.cpp'), JSON.stringify(lines[index]));
        }
        const identifier = lines[18].find((token) => token.text.startsWith('exports'));
        assert.ok(!identifier?.scopes.includes('keyword.control.export.cpp'), JSON.stringify(lines[18]));
    });

    test('bare "module;" colors the keyword', () => {
        const token = findToken(lines[0], 'module');
        assert.ok(token?.scopes.includes('keyword.control.module.cpp'), JSON.stringify(lines[0]));
    });

    test('"export module a.b;" colors export, module, and the dotted name', () => {
        const line = lines[1];
        assert.ok(findToken(line, 'export')?.scopes.includes('keyword.control.export.cpp'));
        assert.ok(findToken(line, 'module')?.scopes.includes('keyword.control.module.cpp'));
        assert.ok(findToken(line, 'a.b')?.scopes.includes('entity.name.namespace.module.cpp'));
    });

    test('"module :private;" colors the bare partition', () => {
        const line = lines[2];
        assert.ok(findToken(line, 'module')?.scopes.includes('keyword.control.module.cpp'));
        assert.ok(findToken(line, ':')?.scopes.includes('punctuation.separator.module-partition.cpp'));
        assert.ok(findToken(line, 'private')?.scopes.includes('entity.name.namespace.module.partition.cpp'));
    });

    test('"import std;" colors the keyword and the module name', () => {
        const line = lines[6];
        assert.ok(findToken(line, 'import')?.scopes.includes('keyword.control.import.cpp'), JSON.stringify(line));
        assert.ok(findToken(line, 'std')?.scopes.includes('entity.name.namespace.module.cpp'));
    });

    test('"import hello.greet;" colors the whole dotted name', () => {
        const line = lines[4];
        assert.ok(findToken(line, 'hello.greet')?.scopes.includes('entity.name.namespace.module.cpp'));
    });

    test('typing "import hello." colors import and hello before the ";" exists', () => {
        const line = lines[5];
        assert.ok(findToken(line, 'import')?.scopes.includes('keyword.control.import.cpp'), JSON.stringify(line));
        assert.ok(findToken(line, 'hello')?.scopes.includes('entity.name.namespace.module.cpp'), JSON.stringify(line));
    });

    test('"export import :part;" colors export, import and the partition', () => {
        const line = lines[3];
        assert.ok(findToken(line, 'export')?.scopes.includes('keyword.control.export.cpp'), JSON.stringify(line));
        assert.ok(findToken(line, 'import')?.scopes.includes('keyword.control.import.cpp'), JSON.stringify(line));
        assert.ok(findToken(line, ':')?.scopes.includes('punctuation.separator.module-partition.cpp'), JSON.stringify(line));
        assert.ok(findToken(line, 'part')?.scopes.includes('entity.name.namespace.module.partition.cpp'), JSON.stringify(line));
    });

    test('"import <vector>;" colors the angle-bracket header', () => {
        const line = lines[7];
        assert.ok(findToken(line, '<vector>')?.scopes.includes('string.quoted.other.header.cpp'));
    });

    test('\'import "foo.h";\' colors the quoted header', () => {
        const line = lines[8];
        assert.ok(findToken(line, '"foo.h"')?.scopes.includes('string.quoted.double.header.cpp'));
    });

    test('"import" used as an ordinary identifier mid-line is never colored as the keyword', () => {
        const assignment = findToken(lines[9], 'import');
        assert.ok(assignment, JSON.stringify(lines[9]));
        assert.ok(!assignment.scopes.includes('keyword.control.import.cpp'), JSON.stringify(lines[9]));

        const memberCall = findToken(lines[10], 'import');
        assert.ok(memberCall, JSON.stringify(lines[10]));
        assert.ok(!memberCall.scopes.includes('keyword.control.import.cpp'), JSON.stringify(lines[10]));
    });

    test('"module" used as an ordinary identifier is never colored as the module keyword', () => {
        const token = findToken(lines[11], 'module');
        assert.ok(token, JSON.stringify(lines[11]));
        assert.ok(!token.scopes.includes('keyword.control.module.cpp'), JSON.stringify(lines[11]));
    });
});
