// Another C++ extension serving the same files shows every result twice.
// Ask once per workspace whether to turn their language features off here.

import * as vscode from 'vscode';
import { DISABLE, KEEP } from './conflictAnswers';
import { askOnce } from './prompt';

// Re-exported so the rest of this file, and existing importers of the
// answers from here (the conflicts scenario's test suite), have one place
// to get them; see conflictAnswers.ts for why they do not simply live here.
export { DISABLE, KEEP };

export const CONFLICT_ANSWER_KEY = 'mcppls.conflictAnswer';

export type ConflictCheck = 'skipped-setting' | 'already-answered' | 'none-found' | 'asked';

interface Conflict {
    extensionId: string;
    displayName: string;
    section: string;
    key: string;
    disabledValue: unknown;
}

const CANDIDATES: readonly Conflict[] = [
    { extensionId: 'ms-vscode.cpptools', displayName: 'C/C++', section: 'C_Cpp', key: 'intelliSenseEngine', disabledValue: 'disabled' },
    { extensionId: 'llvm-vs-code-extensions.vscode-clangd', displayName: 'clangd', section: 'clangd', key: 'enable', disabledValue: false },
];

function activeConflicts(): Conflict[] {
    return CANDIDATES.filter((candidate) => {
        // getExtension answers only for installed extensions that are enabled.
        if (!vscode.extensions.getExtension(candidate.extensionId)) {
            return false;
        }
        const value = vscode.workspace.getConfiguration(candidate.section).get(candidate.key);
        return value !== candidate.disabledValue;
    });
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
    const conflicts = activeConflicts();
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
    if (answer !== DISABLE) {
        return 'asked';
    }
    for (const conflict of conflicts) {
        try {
            await vscode.workspace.getConfiguration(conflict.section)
                .update(conflict.key, conflict.disabledValue, vscode.ConfigurationTarget.Workspace);
            log(`Set ${conflict.section}.${conflict.key} to ${JSON.stringify(conflict.disabledValue)} in the workspace settings.`);
        } catch (error) {
            log(`Could not change ${conflict.section}.${conflict.key}: ${error instanceof Error ? error.message : String(error)}`);
        }
    }
    return 'asked';
}
