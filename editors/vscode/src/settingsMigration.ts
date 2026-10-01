// S-1 (plan 0.0.9): `mcppls.engine` and `mcppls.buildDiscovery` were renamed (see settingsRead.ts).
// Their old values keep working without anyone doing anything; this offers, once per old name, to move
// them -- and writes nothing unless the button is clicked. The extension never edits a person's
// settings.json on its own.

import * as vscode from 'vscode';
import { explicitValue, legacyNamesInUse, RenamedSetting, USER_LAYERS } from './settingsRead';
import { notifyOnce } from './prompt';

const BUTTON = 'Update Settings';
const NOTICE_KEY = 'settingsRenamed';

const TARGETS: Record<(typeof USER_LAYERS)[number], vscode.ConfigurationTarget> = {
    workspaceFolderValue: vscode.ConfigurationTarget.WorkspaceFolder,
    workspaceValue: vscode.ConfigurationTarget.Workspace,
    globalValue: vscode.ConfigurationTarget.Global,
};

/** For every scope that has a value under the old name: the new name gets it, the old name is removed. */
async function moveToNewName(renamed: RenamedSetting): Promise<void> {
    const configuration = vscode.workspace.getConfiguration('mcppls');
    const inspected = configuration.inspect(renamed.legacy);
    for (const layer of USER_LAYERS) {
        const value = inspected?.[layer];
        if (typeof value !== 'string') continue;   // an object here is the new settings' parent, not an old value
        // A value already under the new name in this scope is theirs and stays; only the old one goes.
        if (configuration.inspect(renamed.current)?.[layer] === undefined) {
            await configuration.update(renamed.current, value, TARGETS[layer]);
        }
        await configuration.update(renamed.legacy, undefined, TARGETS[layer]);
    }
}

/**
 * Tells a person, once per old name (remembered in `globalState`), that a setting of theirs was renamed.
 * Their value keeps working either way. `log` gets one line saying what was found.
 */
export async function offerSettingsMigration(context: vscode.ExtensionContext, log: (line: string) => void): Promise<void> {
    const configuration = vscode.workspace.getConfiguration('mcppls');
    for (const renamed of legacyNamesInUse(configuration)) {
        const key = `${NOTICE_KEY}.${renamed.legacy}`;
        if (context.globalState.get<boolean>(key, false)) continue;
        void context.globalState.update(key, true);
        log(`mcppls.${renamed.legacy} is set (${JSON.stringify(explicitValue(configuration.inspect(renamed.legacy)))}); it is now mcppls.${renamed.current}, and the old name still works.`);
        const choice = await notifyOnce('settingsRenamed', 'info',
            `mcppls: the setting mcppls.${renamed.legacy} is now mcppls.${renamed.current}. Your value still applies; update your settings to the new name?`,
            [BUTTON]);
        if (choice === BUTTON) await moveToNewName(renamed);
    }
}
