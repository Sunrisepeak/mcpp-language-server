// The per-workspace off switch against VS Code's settings (`mcppls.enable`, 0.0.7). The decisions
// are in enableSwitch.ts; this reads and writes the settings.

import * as vscode from 'vscode';
import { enabledForFolders, turnOnEdits } from './enableSwitch';

export const TURN_OFF_COMMAND = 'mcppls.turnOffInWorkspace';
export const TURN_ON_COMMAND = 'mcppls.turnOnInWorkspace';

export function serverEnabled(): boolean {
    const folders = vscode.workspace.workspaceFolders ?? [];
    if (folders.length === 0) {
        return vscode.workspace.getConfiguration('mcppls').get<boolean>('enable', true) !== false;
    }
    return enabledForFolders(folders.map((folder) => vscode.workspace.getConfiguration('mcppls', folder.uri).get<boolean>('enable')));
}

// Writes false at the workspace level (the folder's own settings when only one folder is open), so
// the person's user settings are untouched. Returns whether anything was written.
export async function turnOffInWorkspace(log: (line: string) => void): Promise<boolean> {
    const folders = vscode.workspace.workspaceFolders ?? [];
    if (folders.length === 0) {
        void vscode.window.showWarningMessage('C++ Modules: open a folder first; the switch is stored in the workspace\'s settings.');
        return false;
    }
    try {
        await vscode.workspace.getConfiguration('mcppls', folders[0].uri).update('enable', false, vscode.ConfigurationTarget.Workspace);
        log('mcppls.enable set to false in the workspace settings');
        return true;
    } catch (error) {
        log(`mcppls.enable could not be written: ${error instanceof Error ? error.message : String(error)}`);
        void vscode.window.showWarningMessage('C++ Modules: the workspace settings could not be written. Set "mcppls.enable": false there by hand.');
        return false;
    }
}

export async function turnOnInWorkspace(log: (line: string) => void): Promise<boolean> {
    const folders = vscode.workspace.workspaceFolders ?? [];
    let wrote = false;
    try {
        const whole = vscode.workspace.getConfiguration('mcppls', folders[0]?.uri).inspect<boolean>('enable');
        for (const edit of turnOnEdits(whole)) {
            if (edit.scope === 'workspace') {
                await vscode.workspace.getConfiguration('mcppls', folders[0]?.uri).update('enable', edit.value, vscode.ConfigurationTarget.Workspace);
                wrote = true;
            }
        }
        for (const folder of folders) {
            const inspected = vscode.workspace.getConfiguration('mcppls', folder.uri).inspect<boolean>('enable');
            if (inspected?.workspaceFolderValue === false) {
                await vscode.workspace.getConfiguration('mcppls', folder.uri).update('enable', undefined, vscode.ConfigurationTarget.WorkspaceFolder);
                wrote = true;
            }
        }
    } catch (error) {
        log(`mcppls.enable could not be written: ${error instanceof Error ? error.message : String(error)}`);
    }
    log(wrote ? 'mcppls.enable turned back on' : 'mcppls.enable was not off in a settings file this workspace owns');
    return wrote;
}
