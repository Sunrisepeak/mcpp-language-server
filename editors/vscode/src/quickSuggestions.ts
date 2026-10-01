// WA-VSCODE-002 (0.0.8 plan E-1, E-2): completion shows while you type in C and C++.
//
// Since a release between 1.108 and 1.125, VS Code's own default for `editor.quickSuggestions` is
// `{other: "offWhenInlineCompletions"}`: with an inline-completion provider installed (Copilot is built
// into VS Code), a keystroke first waits for the inline completion, the list is not opened at all when it
// shows grey text, and otherwise only after it answered or 750 ms passed with the document and cursor
// unchanged. For a language server that reads as "no completion", and it was measured so (the plan, §1).
// package.json contributes `{other: "on"}` for `[cpp]` and `[c]` instead -- the list and the inline
// completion side by side, as before that release. VS Code ranks a language default above a person's
// setting that names no language, so whoever set `editor.quickSuggestions` for every language loses it in
// C and C++; `overriddenByLanguageDefault` says so, once, in the log, with how to keep theirs.
//
// Plain TypeScript (no `vscode` import), so test/unit runs it in Node.

/** The value package.json contributes as the language default of `editor.quickSuggestions` for C and C++. */
export const QUICK_SUGGESTIONS: Readonly<Record<SuggestionKind, string>> = { other: 'on', comments: 'off', strings: 'off' };

/** The languages it is contributed for: those the extension's document selector serves. */
export const QUICK_SUGGESTION_LANGUAGES: readonly string[] = ['cpp', 'c'];

type SuggestionKind = 'other' | 'comments' | 'strings';
const KINDS: readonly SuggestionKind[] = ['other', 'comments', 'strings'];

/** The fields of `WorkspaceConfiguration.inspect()` this reads (VS Code 1.91 has all of them). */
export interface InspectedValue {
    defaultValue?: unknown;
    globalValue?: unknown;
    workspaceValue?: unknown;
    workspaceFolderValue?: unknown;
    defaultLanguageValue?: unknown;
    globalLanguageValue?: unknown;
    workspaceLanguageValue?: unknown;
    workspaceFolderLanguageValue?: unknown;
}

// VS Code's precedence, highest first: every language-specific value above every other, and within each
// group the folder above the workspace above the user above the default.
const LAYERS: readonly (keyof InspectedValue)[] = [
    'workspaceFolderLanguageValue', 'workspaceLanguageValue', 'globalLanguageValue', 'defaultLanguageValue',
    'workspaceFolderValue', 'workspaceValue', 'globalValue', 'defaultValue',
];

/** The layer the effective value comes from, named as `inspect()` names it without `Value`, or `unset`. */
export function sourceOf(inspected: InspectedValue | undefined): string {
    const layer = LAYERS.find((candidate) => inspected?.[candidate] !== undefined);
    return layer === undefined ? 'unset' : layer.slice(0, -'Value'.length);
}

// A value as VS Code's validation reads it: a boolean or a string applies to all three kinds, an object
// fills the kinds it leaves out from `fallback`, and a boolean inside it means on or off.
function normalize(value: unknown, fallback: Readonly<Record<SuggestionKind, string>>): Record<SuggestionKind, string> {
    const one = (kind: unknown): string | undefined => (typeof kind === 'boolean' ? (kind ? 'on' : 'off') : typeof kind === 'string' ? kind : undefined);
    if (typeof value === 'boolean' || typeof value === 'string') {
        const all = one(value) as string;
        return { other: all, comments: all, strings: all };
    }
    const object = value !== null && typeof value === 'object' ? (value as Record<string, unknown>) : {};
    const normalized = { ...fallback };
    for (const kind of KINDS) {
        normalized[kind] = one(object[kind]) ?? fallback[kind];
    }
    return normalized;
}

const LAYER_NAMES: Readonly<Record<string, string>> = {
    workspaceFolderValue: 'workspace folder setting',
    workspaceValue: 'workspace setting',
    globalValue: 'user setting',
};

/**
 * The line to log when the language default of E-1 is what C++ files get although the person set
 * `editor.quickSuggestions` for every language to something else; undefined when nothing of theirs is
 * overridden (they set nothing, set it for C++ itself, or set what the default is anyway). `inspected` is
 * the setting as `inspect()` sees it for `cpp`; `c` is contributed the same value and read the same way.
 */
export function overriddenByLanguageDefault(inspected: InspectedValue | undefined): string | undefined {
    // Without a language default in effect (an editor that did not apply the contribution), nothing overrides theirs.
    if (!inspected || inspected.defaultLanguageValue === undefined) return undefined;
    const theirsForTheLanguage = inspected.workspaceFolderLanguageValue ?? inspected.workspaceLanguageValue ?? inspected.globalLanguageValue;
    if (theirsForTheLanguage !== undefined) return undefined;
    const layer = (['workspaceFolderValue', 'workspaceValue', 'globalValue'] as const).find((candidate) => inspected[candidate] !== undefined);
    if (layer === undefined) return undefined;
    const vscodeDefault = normalize(inspected.defaultValue, { other: 'on', comments: 'off', strings: 'off' });
    const theirs = normalize(inspected[layer], vscodeDefault);
    const ours = normalize(inspected.defaultLanguageValue, vscodeDefault);
    if (KINDS.every((kind) => theirs[kind] === ours[kind])) return undefined;
    return `editor.quickSuggestions: your ${LAYER_NAMES[layer]} ${JSON.stringify(inspected[layer])} is not what C and C++ files use -- `
        + `this extension sets ${JSON.stringify(ours)} for them so that completion shows while you type `
        + '(VS Code otherwise waits for inline completions such as Copilot\'s first; WA-VSCODE-002). '
        + 'To keep yours, set it under "[cpp]" and "[c]" in the same settings file.';
}
