// The per-workspace off switch (`mcppls.enable`, 0.0.7): the decisions that need no `vscode`.

export interface EnableInspection {
    globalValue?: boolean;
    workspaceValue?: boolean;
    workspaceFolderValue?: boolean;
}

// Only an explicit false turns the server off; anything else (unset, a wrong type) is the default.
export function isEnabled(value: boolean | undefined): boolean {
    return value !== false;
}

// A window with several folders is off only when every folder is: one folder that wants the server
// keeps it, since one server serves the whole window.
export function enabledForFolders(values: readonly (boolean | undefined)[]): boolean {
    return values.length === 0 || values.some(isEnabled);
}

export type TurnOnEdit = { scope: 'workspaceFolder' | 'workspace'; value: true | undefined };

// What "turn it back on" writes. A false set in the workspace or a folder is removed (the default
// applies again and the settings file loses the line). A false in the user's own settings is
// overridden with true at the workspace level, so one click in one project does not rewrite the
// person's default for every project.
export function turnOnEdits(inspection: EnableInspection | undefined): TurnOnEdit[] {
    const edits: TurnOnEdit[] = [];
    if (inspection?.workspaceFolderValue === false) edits.push({ scope: 'workspaceFolder', value: undefined });
    if (inspection?.workspaceValue === false) edits.push({ scope: 'workspace', value: undefined });
    if (edits.length === 0 && inspection?.globalValue === false) edits.push({ scope: 'workspace', value: true });
    return edits;
}

// What to do when the setting changes while the extension runs.
export function transition(wasEnabled: boolean, nowEnabled: boolean): 'start' | 'stop' | 'none' {
    if (wasEnabled === nowEnabled) return 'none';
    return nowEnabled ? 'start' : 'stop';
}
