// Fix plan 2026-09-26 F9 (decision D4, layer 1): the server takes a space as a completion trigger
// character, for the module list after `import`. VS Code would then ask on every space typed
// anywhere; this is what drops those requests before anything is sent. The same rule as the
// server's own gate (src/orchestrator/completion.cppm, is_import_line_prefix), in plain TypeScript
// so it runs without VS Code in the unit tests.

// `^\s*(export\s+)?import\s$`: an import directive's keyword and exactly one blank after it, with
// nothing typed after that.
const IMPORT_LINE_PREFIX = /^[ \t\v\f]*(?:export[ \t\v\f]+)?import[ \t\v\f]$/;

export function isImportLinePrefix(prefix: string): boolean {
    return IMPORT_LINE_PREFIX.test(prefix);
}

// Whether a completion VS Code asks for because `triggerCharacter` was typed at `character` of a
// line should reach the server. Only a space is ever dropped.
export function sendTriggeredCompletion(triggerCharacter: string | undefined, lineText: string, character: number): boolean {
    return triggerCharacter !== ' ' || isImportLinePrefix(lineText.slice(0, character));
}
