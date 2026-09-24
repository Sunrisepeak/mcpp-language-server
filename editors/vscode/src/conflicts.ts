// Another C++ extension serving the same files shows every result twice (design 2026-09-25 §10).
// Once per workspace, this asks whether to turn their language features off here; two commands let
// the user do that (or undo it) at any time, and a listener notices when a conflict becomes active
// after activation and says so once, without asking anything or changing a setting on its own.
//
// Never change another extension's settings without the user choosing it: every path below that
// writes a setting is reached only through an explicit answer to a question or a command the user ran.

import * as vscode from 'vscode';
import {
    activeConflictIds,
    activeSettableConflicts,
    activeUnsettableConflicts,
    ConflictScope,
    SETTABLE_CANDIDATES,
    SettableConflict,
    snapshotForDisable,
    StoredConflictValue,
    UNSETTABLE_CANDIDATES,
    UnsettableConflict,
} from './conflictCandidates';
import { DISABLE, KEEP } from './conflictAnswers';
import { askOnce, pickOnce } from './prompt';

// Re-exported so the rest of this file, and existing importers of the
// answers from here (the conflicts scenario's test suite), have one place
// to get them; see conflictAnswers.ts for why they do not simply live here.
export { DISABLE, KEEP };

export const CONFLICT_ANSWER_KEY = 'mcppls.conflictAnswer';
const STORAGE_KEY = 'mcppls.conflictPreviousValues';

export type ConflictCheck = 'skipped-setting' | 'already-answered' | 'none-found' | 'asked';

function isInstalled(extensionId: string): boolean {
    // getExtension answers only for installed extensions that are enabled.
    return vscode.extensions.getExtension(extensionId) !== undefined;
}

function currentValue(section: string, key: string): unknown {
    return vscode.workspace.getConfiguration(section).get(key);
}

// The override a setting has *at one specific scope*, as opposed to `currentValue`'s effective
// value -- so "there was no override here" is remembered as `undefined`, not as whatever the
// default happens to be, and restoring it means removing the override rather than reapplying a
// value that merely matched today's default.
function valueAtScope(section: string, key: string, scope: ConflictScope): unknown {
    const inspected = vscode.workspace.getConfiguration(section).inspect(key);
    return scope === 'workspace' ? inspected?.workspaceValue : inspected?.globalValue;
}

function storeFor(context: vscode.ExtensionContext, scope: ConflictScope): vscode.Memento {
    return scope === 'workspace' ? context.workspaceState : context.globalState;
}

function targetFor(scope: ConflictScope): vscode.ConfigurationTarget {
    return scope === 'workspace' ? vscode.ConfigurationTarget.Workspace : vscode.ConfigurationTarget.Global;
}

function scopeLabel(scope: ConflictScope): string {
    return scope === 'workspace' ? 'this workspace' : 'user settings';
}

// Writes each conflict's disabled value at `scope`, remembering what was there before so
// `restoreOtherCppFeatures` (or the command's own undo, for restore) can put it back exactly.
async function disableConflicts(
    context: vscode.ExtensionContext,
    conflicts: readonly SettableConflict[],
    scope: ConflictScope,
    log: (line: string) => void,
): Promise<void> {
    if (conflicts.length === 0) {
        return;
    }
    const store = storeFor(context, scope);
    const snapshot = snapshotForDisable(conflicts, (section, key) => valueAtScope(section, key, scope));
    const existing = store.get<StoredConflictValue[]>(STORAGE_KEY) ?? [];
    // A conflict disabled again before being restored keeps its ORIGINAL previous value, not the
    // disabled value it is about to be overwritten with.
    const kept = existing.filter((entry) => !snapshot.some((next) => next.section === entry.section && next.key === entry.key));
    await store.update(STORAGE_KEY, [...kept, ...snapshot]);
    for (const conflict of conflicts) {
        try {
            await vscode.workspace.getConfiguration(conflict.section).update(conflict.key, conflict.disabledValue, targetFor(scope));
            log(`Set ${conflict.section}.${conflict.key} to ${JSON.stringify(conflict.disabledValue)} (${scopeLabel(scope)}).`);
        } catch (error) {
            log(`Could not change ${conflict.section}.${conflict.key}: ${error instanceof Error ? error.message : String(error)}`);
        }
    }
}

// In test mode this still runs every real check (installed extensions,
// settings, the workspaceState guard); only the final question is
// substituted, through askOnce -- see src/prompt.ts for why and how.
export async function checkConflicts(context: vscode.ExtensionContext, log: (line: string) => void): Promise<ConflictCheck> {
    if (!vscode.workspace.getConfiguration('mcppls').get<boolean>('detectConflicts', true)) {
        return 'skipped-setting';
    }
    if (context.workspaceState.get<string>(CONFLICT_ANSWER_KEY) !== undefined) {
        return 'already-answered';
    }
    const conflicts = activeSettableConflicts(isInstalled, currentValue);
    if (conflicts.length === 0) {
        return 'none-found';
    }

    const names = conflicts.map((conflict) => conflict.displayName).join(' and ');
    const verb = conflicts.length === 1 ? 'also provides' : 'also provide';
    const answer = await askOnce(
        'conflict',
        `${names} ${verb} language features for C++ files, so results appear twice. Turn off their language features in this workspace?`,
        DISABLE,
        KEEP,
    );
    // Closing the message counts as an answer too: the question is asked once.
    await context.workspaceState.update(CONFLICT_ANSWER_KEY, answer === DISABLE ? 'disabled' : answer === KEEP ? 'kept' : 'dismissed');
    if (answer === DISABLE) {
        await disableConflicts(context, conflicts, 'workspace', log);
    }
    return 'asked';
}

const SCOPE_ITEMS: readonly { label: string; value: ConflictScope }[] = [
    { label: 'This Workspace', value: 'workspace' },
    { label: 'Everywhere (User Settings)', value: 'global' },
];

// Command: `mcppls: Turn Off Other C++ Language Features`. The first-run question (checkConflicts,
// above) is a shortcut to this, always at workspace scope; this command is the same action, chosen
// deliberately, with the scope the user picks.
export async function turnOffOtherCppFeatures(context: vscode.ExtensionContext, log: (line: string) => void): Promise<void> {
    const scope = await pickOnce('turnOffScope', SCOPE_ITEMS, 'Where to turn off other C++ extensions’ language features');
    if (!scope) {
        return;
    }
    const settable = activeSettableConflicts(isInstalled, currentValue);
    const unsettable = activeUnsettableConflicts(isInstalled);
    if (settable.length === 0 && unsettable.length === 0) {
        void vscode.window.showInformationMessage('mcppls: no other C++ language features are currently active.');
        return;
    }
    await disableConflicts(context, settable, scope, log);
    if (settable.length > 0) {
        void vscode.window.showInformationMessage(
            `mcppls: turned off ${settable.map((conflict) => conflict.displayName).join(' and ')} (${scopeLabel(scope)}).`,
        );
    }
    for (const conflict of unsettable) {
        offerToOpenExtension(conflict, log);
    }
}

// Command: `mcppls: Restore Other C++ Language Features`. Puts back exactly what
// turnOffOtherCppFeatures (or the first-run question) remembered, per scope.
export async function restoreOtherCppFeatures(context: vscode.ExtensionContext, log: (line: string) => void): Promise<void> {
    const scopesWithData = (['workspace', 'global'] as const)
        .filter((scope) => (storeFor(context, scope).get<StoredConflictValue[]>(STORAGE_KEY) ?? []).length > 0);
    if (scopesWithData.length === 0) {
        void vscode.window.showInformationMessage('mcppls: nothing to restore.');
        return;
    }
    let scope: ConflictScope;
    if (scopesWithData.length === 1) {
        scope = scopesWithData[0];
    } else {
        const picked = await pickOnce(
            'restoreScope',
            SCOPE_ITEMS.filter((item) => scopesWithData.includes(item.value)),
            'Which scope to restore',
        );
        if (!picked) {
            return;
        }
        scope = picked;
    }
    const store = storeFor(context, scope);
    const saved = store.get<StoredConflictValue[]>(STORAGE_KEY) ?? [];
    const target = targetFor(scope);
    for (const item of saved) {
        try {
            await vscode.workspace.getConfiguration(item.section).update(item.key, item.value, target);
            log(`Restored ${item.section}.${item.key} to ${JSON.stringify(item.value)} (${scopeLabel(scope)}).`);
        } catch (error) {
            log(`Could not restore ${item.section}.${item.key}: ${error instanceof Error ? error.message : String(error)}`);
        }
    }
    await store.update(STORAGE_KEY, undefined);
    void vscode.window.showInformationMessage(`mcppls: restored other C++ extensions’ language features (${scopeLabel(scope)}).`);
}

function offerToOpenExtension(conflict: UnsettableConflict, log: (line: string) => void): void {
    void vscode.window.showInformationMessage(
        `mcppls: ${conflict.displayName} has no setting to turn off; disable the extension itself from the Extensions view.`,
        'Open Extension',
    ).then((choice) => {
        if (choice !== 'Open Extension') {
            return;
        }
        log(`Opening the Extensions view for ${conflict.extensionId}.`);
        void vscode.commands.executeCommand('workbench.extensions.search', `@id:${conflict.extensionId}`);
    });
}

// A non-modal notice -- an information message, never a warning or error modal -- for a conflict
// that was not active when this session last looked. Shown at most once per conflict per session:
// see watchForNewConflicts below for how "last looked" is tracked.
function notifyNewConflict(context: vscode.ExtensionContext, extensionId: string, log: (line: string) => void): void {
    const settable = SETTABLE_CANDIDATES.find((candidate) => candidate.extensionId === extensionId);
    if (settable) {
        void vscode.window.showInformationMessage(
            `mcppls: ${settable.displayName} is also active: results may appear twice.`,
            'Turn Off',
        ).then((choice) => {
            if (choice === 'Turn Off') {
                void disableConflicts(context, [settable], 'workspace', log);
            }
        });
        return;
    }
    const unsettable = UNSETTABLE_CANDIDATES.find((candidate) => candidate.extensionId === extensionId);
    if (unsettable) {
        offerToOpenExtension(unsettable, log);
    }
}

// Listens for a conflict becoming active after activation -- another C++ extension installed or
// enabled, or its setting turned back on -- and notices once per conflict per session. "Once" is
// tracked as a transition, not a permanent flag: a conflict that goes away and comes back is
// treated as newly active again, while repeated events while nothing has changed (many unrelated
// settings fire onDidChangeConfiguration too) notice nothing.
//
// The very first call only records what is active; it works whichever runs first, this call or an
// event firing on its own, because a transition needs a previous snapshot to compare against, and
// there is deliberately no notice for whatever was already active when this session started
// looking (that was already surfaced, or silently skipped, by checkConflicts above).
export function watchForNewConflicts(context: vscode.ExtensionContext, log: (line: string) => void): vscode.Disposable {
    let known: Set<string> | undefined;

    const recheck = (): void => {
        if (!vscode.workspace.getConfiguration('mcppls').get<boolean>('detectConflicts', true)) {
            return;
        }
        const active = activeConflictIds(isInstalled, currentValue);
        if (known !== undefined) {
            for (const id of active) {
                if (!known.has(id)) {
                    notifyNewConflict(context, id, log);
                }
            }
        }
        known = active;
    };

    recheck();
    return vscode.Disposable.from(
        vscode.extensions.onDidChange(recheck),
        vscode.workspace.onDidChangeConfiguration(recheck),
    );
}
