// Conflict-candidate detection and restore bookkeeping (design 2026-09-25 §10), in plain Node: no
// VS Code, same reason test/unit/serverLog.test.ts is. The `vscode`-dependent glue (conflicts.ts)
// exercises the same logic through the real APIs in the e2e conflicts scenario.
import * as assert from 'assert';
import {
    activeConflictIds,
    activeSettableConflicts,
    activeUnsettableConflicts,
    snapshotForDisable,
    UNSETTABLE_CANDIDATES,
} from '../../src/conflictCandidates';

suite('settable conflict candidates', () => {
    test('not installed is never a conflict', () => {
        assert.deepStrictEqual(activeSettableConflicts(() => false, () => 'default'), []);
    });

    test('installed and at its enabled value is a conflict', () => {
        const found = activeSettableConflicts(() => true, () => 'default');
        assert.strictEqual(found.length, 2);
        assert.deepStrictEqual(found.map((c) => c.extensionId), ['ms-vscode.cpptools', 'llvm-vs-code-extensions.vscode-clangd']);
    });

    test('installed but already at its disabled value is not a conflict', () => {
        const currentValue = (section: string, key: string): unknown => {
            if (section === 'C_Cpp' && key === 'intelliSenseEngine') return 'disabled';
            if (section === 'clangd' && key === 'enable') return false;
            return undefined;
        };
        assert.deepStrictEqual(activeSettableConflicts(() => true, currentValue), []);
    });

    test('one installed and off, the other installed and on: only the active one is a conflict', () => {
        const currentValue = (section: string): unknown => (section === 'clangd' ? false : 'default');
        const found = activeSettableConflicts(() => true, currentValue);
        assert.strictEqual(found.length, 1);
        assert.strictEqual(found[0].extensionId, 'ms-vscode.cpptools');
    });
});

suite('unsettable conflict candidates', () => {
    test('ccls, with no enable setting, is a candidate', () => {
        assert.deepStrictEqual(UNSETTABLE_CANDIDATES.map((c) => c.extensionId), ['ccls-project.ccls']);
    });

    test('active only when installed', () => {
        assert.deepStrictEqual(activeUnsettableConflicts(() => false), []);
        assert.strictEqual(activeUnsettableConflicts(() => true).length, 1);
    });
});

suite('activeConflictIds', () => {
    test('combines settable and unsettable ids', () => {
        const ids = activeConflictIds(() => true, () => 'default');
        assert.deepStrictEqual(
            [...ids].sort(),
            ['ccls-project.ccls', 'llvm-vs-code-extensions.vscode-clangd', 'ms-vscode.cpptools'].sort(),
        );
    });

    test('empty when nothing is installed', () => {
        assert.deepStrictEqual(activeConflictIds(() => false, () => 'default'), new Set());
    });
});

suite('snapshotForDisable (restore bookkeeping)', () => {
    test('remembers each conflict\'s current override at the scope being turned off', () => {
        const conflicts = activeSettableConflicts(() => true, () => 'default');
        const valueAtScope = (section: string, key: string): unknown => (section === 'clangd' && key === 'enable' ? true : undefined);
        const snapshot = snapshotForDisable(conflicts, valueAtScope);
        assert.deepStrictEqual(snapshot, [
            { section: 'C_Cpp', key: 'intelliSenseEngine', value: undefined },
            { section: 'clangd', key: 'enable', value: true },
        ]);
    });

    test('no override at that scope is remembered as undefined, not as a default value', () => {
        const conflicts = activeSettableConflicts(() => true, () => 'default');
        const snapshot = snapshotForDisable(conflicts, () => undefined);
        assert.ok(snapshot.every((entry) => entry.value === undefined));
    });
});
