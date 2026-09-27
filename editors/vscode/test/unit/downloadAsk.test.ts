// When the extension offers to fetch what the build description needs (plan 2026-09-27 B-2, §9.2), in
// plain Node: no VS Code, same reason test/unit/statusText.test.ts is.
import * as assert from 'assert';
import { needsDownload, shouldAsk } from '../../src/downloadAsk';

suite('the build description download offer', () => {
    const issue = { code: 'producer-needs-download', message: 'xim:qt-base@6.11.1 is not installed', askOnline: true };

    test('offered when the server lets a client offer it', () => {
        assert.strictEqual(shouldAsk(issue, false, [], false), true);
    });

    test('not offered when the server does not (an older server, askBeforeDownload off, a fetch already running)', () => {
        assert.strictEqual(shouldAsk({ ...issue, askOnline: false }, false, [], false), false);
        assert.strictEqual(shouldAsk({ code: issue.code, message: issue.message }, false, [], false), false);
    });

    test('never again after "Don\'t Ask Again", and once per set of missing things', () => {
        assert.strictEqual(shouldAsk(issue, true, [], false), false);
        assert.strictEqual(shouldAsk(issue, false, [issue.message], false), false);
        assert.strictEqual(shouldAsk({ ...issue, message: 'fmt is not populated' }, false, [issue.message], false), true);
    });

    test('one open question per root', () => {
        assert.strictEqual(shouldAsk(issue, false, [], true), false);
    });

    test('found among the status issues, absent once the build description no longer needs it', () => {
        assert.deepStrictEqual(needsDownload({ issues: [{ code: 'model-stale', message: 'x' }, issue] }), issue);
        assert.strictEqual(needsDownload({ issues: [{ code: 'model-stale', message: 'x' }] }), undefined);
        assert.strictEqual(needsDownload({}), undefined);
    });
});
