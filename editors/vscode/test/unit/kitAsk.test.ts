import * as assert from 'assert';
import { kitAction, kitInstallToTell, kitKey, kitMissing, KitIssue } from '../../src/kitAsk';

suite('the semantic kit of a target (ecosystem design §9.2)', () => {
    const issue: KitIssue = {
        code: 'kit-missing',
        message: 'the semantic kit for x86_64-linux-gnu is not installed',
        kit: { target: 'x86_64-linux-gnu', package: 'mcppls-kit-x86_64-linux-gnu', version: '23.1.0', command: 'xlings install mcppls-kit-x86_64-linux-gnu@23.1.0' },
        askInstall: true,
    };

    test('the kit-missing issue is found among the status issues', () => {
        assert.strictEqual(kitMissing({ issues: [{ code: 'model-stale', message: 'x' }, issue] }), issue);
        assert.strictEqual(kitMissing({ issues: [] }), undefined);
        assert.strictEqual(kitMissing({}), undefined);
    });

    test('asked once per kit, installed without asking only on the person\'s say-so', () => {
        assert.strictEqual(kitAction(issue, false, false, [], false), 'ask');
        assert.strictEqual(kitAction(issue, false, true, [], false), 'install');
        assert.strictEqual(kitAction(issue, true, false, [], false), 'none', 'Don\'t Ask Again');
        assert.strictEqual(kitAction(issue, false, false, [kitKey(issue)], false), 'none', 'this kit was asked about');
        assert.strictEqual(kitAction(issue, false, true, [kitKey(issue)], false), 'none', 'installed once per kit');
        assert.strictEqual(kitAction(issue, false, false, [], true), 'none', 'a question about this root is open');
        assert.strictEqual(kitAction({ ...issue, askInstall: false }, false, true, [], false), 'none', 'the server cannot install it (an install runs, or no xlings)');
        assert.strictEqual(kitAction(undefined, false, true, [], false), 'none');
        assert.notStrictEqual(kitKey(issue), kitKey({ ...issue, kit: { ...issue.kit!, version: '23.1.1' } }), 'another version is another question');
    });

    test('how an install ended is told once', () => {
        const kitInstall = { outcome: 'installed' as const, target: 'x86_64-linux-gnu', message: 'mcppls-kit-x86_64-linux-gnu@23.1.0 is installed', at: '2026-10-30T00:00:00Z' };
        assert.deepStrictEqual(kitInstallToTell({ kitInstall }, []), kitInstall);
        assert.strictEqual(kitInstallToTell({ kitInstall }, [kitInstall.at]), undefined);
        assert.strictEqual(kitInstallToTell({}, []), undefined);
    });
});
