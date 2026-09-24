// The extension's own workaround registry (design 2026-09-25 §9), mirroring the server's
// (src/engine/clangd/workarounds.cppm, a parallel track): one entry per fix that exists only
// because of a bug in someone else's code, so it is grep-able, named at startup, and has a canary
// test that fails once it can be removed.
//
// The one entry here today is WA-VSCODE-001: VS Code's own C++ grammar defines a `module_import`
// rule but never includes it from anywhere, so `import std;` gets no keyword scope at all (measured
// on better-cpp-syntax 071dd6e -- see the design doc, §7). syntaxes/mcppls-modules.tmLanguage.json
// is the fix: an injection grammar that colors the module syntax VS Code's grammar misses. Remove
// both this entry and that grammar file once the built-in grammar scopes `import std;` itself; the
// canary in test/suite/workaroundCanary.test.ts fails first, naming this id, when that happens.
//
// (JSON grammars cannot carry comments, which is why this note lives here rather than beside the
// `include` it is about.)

export interface Workaround {
    readonly id: string;
    readonly title: string;
    readonly affects: string;
    readonly upstream: string;
    readonly evidence: string;
    readonly added: string;
    readonly removeWhen: string;
    readonly canary: string;
}

export const WORKAROUNDS: readonly Workaround[] = [
    {
        id: 'WA-VSCODE-001',
        title: "an injected grammar colors `import`/`module`/`export` and module names, which VS Code's own cpp grammar does not",
        affects: "VS Code's built-in cpp grammar (better-cpp-syntax 071dd6e): it defines module_import but no rule includes it",
        upstream: 'unfiled',
        evidence: '.agents/docs/2026-09-25-import-hang-status-highlight.md#7',
        added: '0.0.4, 2026-09-25',
        removeWhen: "the built-in cpp grammar scopes `import std;` itself",
        canary: 'test/suite/workaroundCanary.test.ts: fails once module_import is referenced by an include',
    },
];

// One line at activation naming the active workarounds, so a bug report shows which ones were on
// (design §9: "Visible"). Every entry here is unconditional today (there is exactly one, and it is
// not gated by any version), unlike the server's, which gates by the running engine's version.
export function describeActiveWorkarounds(): string {
    if (WORKAROUNDS.length === 0) {
        return 'No workarounds active.';
    }
    return `Active workarounds: ${WORKAROUNDS.map((workaround) => workaround.id).join(', ')}.`;
}
