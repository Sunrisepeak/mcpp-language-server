// Layer 2 of highlighting `import` (design 2026-09-25 §7, §12 "Shared contracts"): the server's own
// semantic tokens for module syntax -- a `keyword` type for `import`/`module`/`export` and a custom
// `module` type (declared by this extension in package.json's `semanticTokenTypes`, with a
// `namespace` fallback) for module and partition names.
//
// NEEDS THE NEW SERVER: mcppls 0.0.3 (the payload this checkout builds against today) has no
// `keyword` or `module` entry in its semantic token legend at all -- confirmed live before writing
// this file: clangd's legend carries only its own C++ symbol kinds. Every assertion below is
// skipped, not failed, until a 0.0.4 server adds them; see MCPPLS_SERVER / MCPPLS_PAYLOAD in
// test/runTest.ts's header to point this suite at one once it exists.

import * as assert from 'assert';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 120_000;

interface Legend {
    tokenTypes: string[];
    tokenModifiers: string[];
}

interface DecodedToken {
    line: number;
    startChar: number;
    length: number;
    type: string;
    modifiers: string[];
}

// The LSP `SemanticTokens.data` delta encoding: repeating groups of
// [deltaLine, deltaStartChar, length, tokenTypeIndex, tokenModifiersBitset].
function decode(data: ArrayLike<number>, legend: Legend): DecodedToken[] {
    const tokens: DecodedToken[] = [];
    let line = 0;
    let char = 0;
    for (let i = 0; i + 4 < data.length + 1; i += 5) {
        const deltaLine = data[i];
        const deltaStart = data[i + 1];
        const length = data[i + 2];
        const typeIndex = data[i + 3];
        const modifierBits = data[i + 4];
        line += deltaLine;
        char = deltaLine === 0 ? char + deltaStart : deltaStart;
        const modifiers = legend.tokenModifiers.filter((_, bit) => (modifierBits & (1 << bit)) !== 0);
        tokens.push({ line, startChar: char, length, type: legend.tokenTypes[typeIndex] ?? `#${typeIndex}`, modifiers });
    }
    return tokens;
}

function tokensOnLine(tokens: readonly DecodedToken[], line: number): DecodedToken[] {
    return tokens.filter((token) => token.line === line);
}

suite('module-syntax highlighting: server semantic tokens (design §7 layer 2)', function () {
    this.timeout(READY_TIMEOUT_MS + 30_000);

    let legend: Legend | undefined;
    let tokens: DecodedToken[] = [];

    suiteSetup(async function () {
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        const api = await extension.activate();
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);

        const folder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(folder, 'the fixture workspace is not open');
        const uri = vscode.Uri.joinPath(folder.uri, 'src', 'main.cpp');
        const document = await vscode.workspace.openTextDocument(uri);
        await vscode.window.showTextDocument(document);

        legend = await vscode.commands.executeCommand<Legend>('vscode.provideDocumentSemanticTokensLegend', uri);
        if (!legend || !legend.tokenTypes.includes('keyword') || !legend.tokenTypes.includes('module')) {
            // The precondition this whole suite needs; every test below skips instead of failing.
            legend = undefined;
            return;
        }
        const raw = await vscode.commands.executeCommand<vscode.SemanticTokens>('vscode.provideDocumentSemanticTokens', uri);
        tokens = raw ? decode(raw.data, legend) : [];
    });

    // Fixture (test/runTest.ts / conformance/fixtures/inferred/src/main.cpp):
    //   line 0: import std;
    //   line 1: import hello.greet;

    test('"import" on line 0 is the "keyword" type', function () {
        if (!legend) {
            this.skip();
            return;
        }
        const line = tokensOnLine(tokens, 0);
        const keyword = line.find((token) => token.type === 'keyword');
        assert.ok(keyword, `no "keyword" token on line 0: ${JSON.stringify(line)}`);
    });

    test('"std" on line 0 is the "module" type', function () {
        if (!legend) {
            this.skip();
            return;
        }
        const line = tokensOnLine(tokens, 0);
        const module = line.find((token) => token.type === 'module');
        assert.ok(module, `no "module" token on line 0: ${JSON.stringify(line)}`);
    });

    test('"import" on line 1 is the "keyword" type, and "hello.greet" is "module"', function () {
        if (!legend) {
            this.skip();
            return;
        }
        const line = tokensOnLine(tokens, 1);
        assert.ok(line.some((token) => token.type === 'keyword'), `no "keyword" token on line 1: ${JSON.stringify(line)}`);
        assert.ok(line.some((token) => token.type === 'module'), `no "module" token on line 1: ${JSON.stringify(line)}`);
    });

    // mcpp design 2026-10-28 §11.1 (H1): after an edit, the tokens are answered at once from what the compiler last gave,
    // moved with the edit, instead of after the pause in typing and the rebuild; the server's report counts the answer.
    // main.cpp is not a module unit, so the compiler is held to the pause (300 ms at least) before it builds it again.
    test('an edit is answered at once with the compiler\'s tokens moved, not after the rebuild', async function () {
        if (!legend) {
            this.skip();
            return;
        }
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID)!;
        const api = await extension.activate();
        const folder = vscode.workspace.workspaceFolders![0];
        const uri = vscode.Uri.joinPath(folder.uri, 'src', 'main.cpp');
        const document = await vscode.workspace.openTextDocument(uri);
        const editor = await vscode.window.showTextDocument(document);
        const compilerToken = (token: DecodedToken) => !['keyword', 'module', 'namespace'].includes(token.type);
        // The compiler's own tokens first: the line `std::println(... hello::greet(...))` has a function.
        const printLine = document.getText().split('\n').findIndex((line) => line.includes('greet('));
        assert.ok(printLine > 0, 'main.cpp calls greet');
        const deadline = Date.now() + 60_000;
        for (;;) {
            const raw = await vscode.commands.executeCommand<vscode.SemanticTokens>('vscode.provideDocumentSemanticTokens', uri);
            if (raw && tokensOnLine(decode(raw.data, legend), printLine).some(compilerToken)) break;
            assert.ok(Date.now() < deadline, `the compiler never coloured line ${printLine} of main.cpp`);
            await new Promise((resolve) => setTimeout(resolve, 300));
        }
        const before = (await api.serverReport()) as { roots?: { semanticTokens?: { answers?: { standIn?: number } } }[] } | undefined;
        const standInBefore = before?.roots?.[0]?.semanticTokens?.answers?.standIn ?? 0;
        try {
            assert.ok(await editor.edit((builder) => builder.insert(new vscode.Position(printLine, 0), '    int added = 0;\n')));
            const started = Date.now();
            const raw = await vscode.commands.executeCommand<vscode.SemanticTokens>('vscode.provideDocumentSemanticTokens', uri);
            const took = Date.now() - started;
            const moved = raw ? tokensOnLine(decode(raw.data, legend), printLine + 1) : [];
            assert.ok(moved.some(compilerToken), `no compiler token moved to line ${printLine + 1}: ${JSON.stringify(moved)}`);
            const after = (await api.serverReport()) as typeof before;
            const standInAfter = after?.roots?.[0]?.semanticTokens?.answers?.standIn ?? 0;
            assert.ok(standInAfter > standInBefore, `the server did not answer from what it kept (standIn ${standInBefore} -> ${standInAfter}, ${took} ms)`);
        } finally {
            await vscode.commands.executeCommand('undo');
            if (document.isDirty) await document.save();
        }
    });

    test('no two tokens overlap', function () {
        if (!legend) {
            this.skip();
            return;
        }
        const byLine = new Map<number, DecodedToken[]>();
        for (const token of tokens) {
            byLine.set(token.line, [...(byLine.get(token.line) ?? []), token]);
        }
        for (const [line, lineTokens] of byLine) {
            const sorted = [...lineTokens].sort((a, b) => a.startChar - b.startChar);
            for (let i = 1; i < sorted.length; i += 1) {
                assert.ok(
                    sorted[i].startChar >= sorted[i - 1].startChar + sorted[i - 1].length,
                    `overlapping tokens on line ${line}: ${JSON.stringify(sorted[i - 1])} and ${JSON.stringify(sorted[i])}`,
                );
            }
        }
    });
});
