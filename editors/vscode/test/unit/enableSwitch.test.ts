// The per-workspace off switch: only an explicit false turns the server off.
import * as assert from 'assert';
import { enabledForFolders, isEnabled, transition, turnOnEdits } from '../../src/enableSwitch';

suite('enable switch', () => {
    test('only an explicit false is off', () => {
        assert.strictEqual(isEnabled(undefined), true);
        assert.strictEqual(isEnabled(true), true);
        assert.strictEqual(isEnabled(false), false);
        assert.strictEqual(isEnabled('no' as unknown as boolean), true);
    });

    test('a multi-folder window is off only when every folder is', () => {
        assert.strictEqual(enabledForFolders([]), true);
        assert.strictEqual(enabledForFolders([false, false]), false);
        assert.strictEqual(enabledForFolders([false, undefined]), true);
    });

    test('turning it on removes a workspace false and overrides a user-level one at the workspace', () => {
        assert.deepStrictEqual(turnOnEdits({ workspaceValue: false }), [{ scope: 'workspace', value: undefined }]);
        assert.deepStrictEqual(turnOnEdits({ workspaceFolderValue: false, workspaceValue: false }), [
            { scope: 'workspaceFolder', value: undefined }, { scope: 'workspace', value: undefined }]);
        assert.deepStrictEqual(turnOnEdits({ globalValue: false }), [{ scope: 'workspace', value: true }]);
        assert.deepStrictEqual(turnOnEdits({ globalValue: false, workspaceValue: false }), [{ scope: 'workspace', value: undefined }]);
        assert.deepStrictEqual(turnOnEdits({}), []);
        assert.deepStrictEqual(turnOnEdits(undefined), []);
    });

    test('a change at runtime starts or stops, and no change does nothing', () => {
        assert.strictEqual(transition(true, false), 'stop');
        assert.strictEqual(transition(false, true), 'start');
        assert.strictEqual(transition(true, true), 'none');
        assert.strictEqual(transition(false, false), 'none');
    });
});
