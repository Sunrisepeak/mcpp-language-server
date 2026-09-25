// Which other C++ extensions conflict with this one, and the pure part of deciding whether one is
// currently active. Kept free of `vscode` so the decision logic (design 2026-09-25 §10) is testable
// in plain Node, the same reason src/serverLog.ts is.
//
// Two shapes, because VS Code gives no way to disable a language feature that has no setting for
// it: a `SettableConflict` is turned off by writing its setting; an `UnsettableConflict` (for
// example ccls, which has no "enable" toggle) can only be pointed out, so the user disables the
// extension itself from the Extensions view.

export interface SettableConflict {
    readonly extensionId: string;
    readonly displayName: string;
    readonly section: string;
    readonly key: string;
    readonly disabledValue: unknown;
}

export interface UnsettableConflict {
    readonly extensionId: string;
    readonly displayName: string;
}

// mcpp-vscode (mcpp-community.mcpp-vscode) is deliberately absent: it has no language server, and
// its module grammar uses the same scopes as this extension's own, so nothing is shown twice.
export const SETTABLE_CANDIDATES: readonly SettableConflict[] = [
    { extensionId: 'ms-vscode.cpptools', displayName: 'C/C++', section: 'C_Cpp', key: 'intelliSenseEngine', disabledValue: 'disabled' },
    { extensionId: 'llvm-vs-code-extensions.vscode-clangd', displayName: 'clangd', section: 'clangd', key: 'enable', disabledValue: false },
];

export const UNSETTABLE_CANDIDATES: readonly UnsettableConflict[] = [
    { extensionId: 'ccls-project.ccls', displayName: 'ccls' },
];

// `isInstalled` and `currentValue` are the two things only VS Code can answer (installed *and*
// enabled extensions; the effective setting value); everything else here is a pure decision.
export function activeSettableConflicts(
    isInstalled: (extensionId: string) => boolean,
    currentValue: (section: string, key: string) => unknown,
): SettableConflict[] {
    return SETTABLE_CANDIDATES.filter(
        (candidate) => isInstalled(candidate.extensionId) && currentValue(candidate.section, candidate.key) !== candidate.disabledValue,
    );
}

export function activeUnsettableConflicts(isInstalled: (extensionId: string) => boolean): UnsettableConflict[] {
    return UNSETTABLE_CANDIDATES.filter((candidate) => isInstalled(candidate.extensionId));
}

// The ids of every conflict active right now, settable or not -- what "a newly active conflict"
// (design §10) is compared against from one recheck to the next.
export function activeConflictIds(
    isInstalled: (extensionId: string) => boolean,
    currentValue: (section: string, key: string) => unknown,
): Set<string> {
    const ids = new Set<string>();
    for (const conflict of activeSettableConflicts(isInstalled, currentValue)) {
        ids.add(conflict.extensionId);
    }
    for (const conflict of activeUnsettableConflicts(isInstalled)) {
        ids.add(conflict.extensionId);
    }
    return ids;
}

// --- Restore bookkeeping ----------------------------------------------------
//
// What "turn off" remembers before it changes a setting, so "restore" can put exactly that back --
// including "there was no override at this scope", which is `value: undefined` and means the
// restore removes the override rather than writing a value that merely matches today's default.

export type ConflictScope = 'workspace' | 'global';

export interface StoredConflictValue {
    readonly section: string;
    readonly key: string;
    readonly value: unknown;
}

// `valueAtScope` reads the setting's own override at exactly the scope being turned off (VS Code's
// `inspect(key).workspaceValue` / `.globalValue`), not the effective value `.get()` would return,
// so a setting with no override here is remembered as `undefined` rather than as its default.
export function snapshotForDisable(
    conflicts: readonly SettableConflict[],
    valueAtScope: (section: string, key: string) => unknown,
): StoredConflictValue[] {
    return conflicts.map((conflict) => ({ section: conflict.section, key: conflict.key, value: valueAtScope(conflict.section, conflict.key) }));
}
