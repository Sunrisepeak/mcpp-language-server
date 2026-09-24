// Canary for WA-VSCODE-001 (design 2026-09-25 §9, src/workarounds.ts): the injection grammar in
// syntaxes/mcppls-modules.tmLanguage.json exists only because VS Code's own built-in cpp grammar
// defines a `module_import` repository rule but no `include` anywhere in the grammar actually uses
// it, so `import std;` gets no keyword scope from VS Code alone. This fails on purpose, naming the
// workaround, the moment that stops being true -- which is the signal to remove both the workaround
// entry and the grammar file (or at least the parts of it VS Code has taken over).

import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

interface TmGrammar {
    repository?: Record<string, unknown>;
}

// The shipped grammar's own build step prefixes repository keys with a hash to avoid collisions
// when grammars are merged (measured: `module_import` ships as something like
// `d9bc4796b0b_module_import`), so both the key's existence and its use are matched by suffix
// rather than by the exact name the design doc found in the (unmangled) upstream source.
function isModuleImportKey(key: string): boolean {
    return key === 'module_import' || key.endsWith('_module_import');
}

// Every `"include": "..."` string anywhere in the grammar, found structurally rather than by
// scanning the raw text, since `#module_import` could otherwise appear inside a comment, another
// rule's `match`, or a `name` and be mistaken for a real reference.
function includedRepositoryKeys(grammar: unknown, into: Set<string> = new Set()): Set<string> {
    if (Array.isArray(grammar)) {
        for (const item of grammar) {
            includedRepositoryKeys(item, into);
        }
    } else if (grammar && typeof grammar === 'object') {
        for (const [key, value] of Object.entries(grammar as Record<string, unknown>)) {
            if (key === 'include' && typeof value === 'string' && value.startsWith('#')) {
                into.add(value.slice(1));
            } else {
                includedRepositoryKeys(value, into);
            }
        }
    }
    return into;
}

test('WA-VSCODE-001 canary: the built-in cpp grammar still does not include module_import', async function () {
    this.timeout(30_000);
    const grammarPath = path.join(vscode.env.appRoot, 'extensions', 'cpp', 'syntaxes', 'cpp.tmLanguage.json');
    const raw = fs.readFileSync(grammarPath, 'utf8');
    const grammar = JSON.parse(raw) as TmGrammar;
    const repositoryKeys = Object.keys(grammar.repository ?? {});
    assert.ok(
        repositoryKeys.some(isModuleImportKey),
        'the built-in grammar no longer defines a module_import rule at all; re-check this canary by hand',
    );

    const included = [...includedRepositoryKeys(grammar)];
    assert.ok(
        !included.some(isModuleImportKey),
        'WA-VSCODE-001 is no longer needed: remove the injection grammar '
        + '(syntaxes/mcppls-modules.tmLanguage.json) and its entry in src/workarounds.ts -- '
        + "VS Code's own cpp grammar now includes module_import.",
    );
});
