// Seeded random use through VS Code's own provider commands (real-project plan RP0): open the
// workspace's C++ files in a random order, and at random identifiers ask hover, definition,
// references, completion and document symbols, as a person clicking around would. Every request
// must come back (no error, none past its budget) and the slow ones must stay rare.

import * as assert from 'assert';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 120_000;
const ACTIONS = Number(process.env.MCPPLS_STRESS_ACTIONS ?? 40);
const SEED = Number(process.env.MCPPLS_STRESS_SEED ?? 7);
const REQUEST_BUDGET_MS = Number(process.env.MCPPLS_STRESS_REQUEST_MS ?? 15_000);
const P90_BUDGET_MS = Number(process.env.MCPPLS_STRESS_P90_MS ?? 5_000);

// mulberry32: small, seedable, the same sequence on every platform.
function random(seed: number): () => number {
    let state = seed >>> 0;
    return () => {
        state = (state + 0x6d2b79f5) >>> 0;
        let t = state;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

type Kind = 'hover' | 'definition' | 'references' | 'completion' | 'documentSymbol';
const KINDS: readonly Kind[] = ['hover', 'definition', 'references', 'completion', 'documentSymbol'];
const SKIP = new Set(['import', 'export', 'module', 'return', 'const', 'auto', 'std', 'int', 'char', 'void', 'include']);

function identifiers(document: vscode.TextDocument): vscode.Position[] {
    const spots: vscode.Position[] = [];
    for (let line = 0; line < document.lineCount; line++) {
        const text = document.lineAt(line).text;
        if (/^\s*(\/\/|#)/.test(text)) continue;
        for (const match of text.matchAll(/[A-Za-z_]\w{2,}/g)) {
            if (!SKIP.has(match[0])) spots.push(new vscode.Position(line, match.index! + Math.floor(match[0].length / 2)));
        }
    }
    return spots;
}

async function ask(kind: Kind, uri: vscode.Uri, at: vscode.Position): Promise<unknown> {
    switch (kind) {
        case 'hover': return vscode.commands.executeCommand('vscode.executeHoverProvider', uri, at);
        case 'definition': return vscode.commands.executeCommand('vscode.executeDefinitionProvider', uri, at);
        case 'references': return vscode.commands.executeCommand('vscode.executeReferenceProvider', uri, at);
        case 'completion': return vscode.commands.executeCommand('vscode.executeCompletionItemProvider', uri, at);
        case 'documentSymbol': return vscode.commands.executeCommand('vscode.executeDocumentSymbolProvider', uri);
    }
}

suite('C++ modules under random use', function () {
    let api: TestApi;

    suiteSetup(async function () {
        this.timeout(READY_TIMEOUT_MS + 30_000);
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
        const first = await vscode.workspace.findFiles('**/*.{cpp,cppm}', undefined, 1);
        assert.ok(first.length > 0, 'the workspace has no C++ files');
        await vscode.window.showTextDocument(first[0]);
        await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
    });

    test(`${ACTIONS} random requests, seed ${SEED}: every one answered within its budget`, async function () {
        this.timeout(ACTIONS * REQUEST_BUDGET_MS + 60_000);
        const next = random(SEED);
        const files = (await vscode.workspace.findFiles('**/*.{cpp,cppm}')).sort((a, b) => a.fsPath.localeCompare(b.fsPath));
        const latencies: number[] = [];
        const failures: string[] = [];
        let document = await vscode.workspace.openTextDocument(files[0]);
        for (let action = 0; action < ACTIONS; action++) {
            if (next() < 0.3) {
                document = await vscode.workspace.openTextDocument(files[Math.floor(next() * files.length)]);
                await vscode.window.showTextDocument(document, { preview: true, preserveFocus: true });
            }
            const spots = identifiers(document);
            if (spots.length === 0) continue;
            const at = spots[Math.floor(next() * spots.length)];
            const kind = KINDS[Math.floor(next() * KINDS.length)];
            const where = `${kind} ${vscode.workspace.asRelativePath(document.uri)}:${at.line + 1}:${at.character + 1}`;
            const started = Date.now();
            try {
                const outcome = await Promise.race([
                    ask(kind, document.uri, at).then(() => 'answered' as const),
                    new Promise<'timeout'>((resolve) => setTimeout(() => resolve('timeout'), REQUEST_BUDGET_MS)),
                ]);
                if (outcome === 'timeout') failures.push(`${where}: no answer within ${REQUEST_BUDGET_MS} ms`);
                latencies.push(Date.now() - started);
            } catch (error) {
                failures.push(`${where}: ${error instanceof Error ? error.message : String(error)}`);
            }
        }
        latencies.sort((a, b) => a - b);
        const p90 = latencies.length > 0 ? latencies[Math.min(latencies.length - 1, Math.ceil(latencies.length * 0.9) - 1)] : 0;
        console.log(`stress: ${latencies.length} answered, ${failures.length} failed, p90 ${p90} ms, max ${latencies.at(-1) ?? 0} ms`);
        assert.deepStrictEqual(failures, [], 'every request is answered, without error, within its budget');
        assert.ok(p90 <= P90_BUDGET_MS, `p90 ${p90} ms is over the ${P90_BUDGET_MS} ms budget`);
    });
});
