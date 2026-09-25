// The editor side of the space trigger (fix plan 2026-09-26 F9, D4 layer 1), in plain Node: no VS
// Code, same reason test/unit/serverLog.test.ts is. The cases are the server's own
// (tests/test_completion.cpp), so the two gates cannot drift apart unnoticed.
import * as assert from 'assert';
import { isImportLinePrefix, sendTriggeredCompletion } from '../../src/completionGate';

suite('space-triggered completion', () => {
    test('an import directive keyword and one blank passes', () => {
        for (const prefix of ['import ', 'export import ', '  import ', '\timport\t', 'export  import ', '\texport\timport ']) {
            assert.strictEqual(isImportLinePrefix(prefix), true, JSON.stringify(prefix));
        }
    });

    test('anything else does not', () => {
        for (const prefix of ['import  ', 'import', 'import a', 'import a ', 'int x = ', '', ' ', 'exportimport ', 'export ',
            'export module ', 'module ', 'importer ', '// import ', 'x import ']) {
            assert.strictEqual(isImportLinePrefix(prefix), false, JSON.stringify(prefix));
        }
    });

    test('only a space is ever dropped, and only off an import line', () => {
        assert.strictEqual(sendTriggeredCompletion(' ', 'int x = 1;', 8), false);
        assert.strictEqual(sendTriggeredCompletion(' ', 'import hello;', 7), true, 'the line before the cursor is what counts');
        assert.strictEqual(sendTriggeredCompletion('.', 'import hello.', 13), true);
        assert.strictEqual(sendTriggeredCompletion(undefined, 'int x = ', 8), true, 'typed or invoked completion always goes');
    });
});
