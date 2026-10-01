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

// D-4, D-5 (plan 0.0.9): downloads allowed for a workspace, and the outcome of a fetch told once.
import { downloadAction, onlineRunToTell } from '../../src/downloadAsk';

suite('downloads allowed in a workspace, and what a fetch came to', () => {
    const issue = { code: 'producer-needs-download', message: 'xmake needs libtool downloaded', askOnline: true };

    test('fetched without asking once allowed, asked otherwise, nothing after "Don\'t Ask Again"', () => {
        assert.strictEqual(downloadAction(issue, false, true, [], false), 'fetch');
        assert.strictEqual(downloadAction(issue, false, false, [], false), 'ask');
        assert.strictEqual(downloadAction(issue, true, false, [], false), 'none');
        assert.strictEqual(downloadAction(issue, true, true, [], false), 'fetch', 'allowing is the newer answer');
    });

    test('never for a run the server does not offer, a set of missing things already handled, or an open question', () => {
        assert.strictEqual(downloadAction({ ...issue, askOnline: false }, false, true, [], false), 'none');
        assert.strictEqual(downloadAction(issue, false, true, [issue.message], false), 'none');
        assert.strictEqual(downloadAction(issue, false, true, [], true), 'none');
        assert.strictEqual(downloadAction(undefined, false, true, [], false), 'none');
    });

    test('each fetch is told once, and only a known outcome', () => {
        const run = { outcome: 'failed' as const, message: 'xmake could not install libtool', at: '2026-10-01T10:00:00Z' };
        assert.deepStrictEqual(onlineRunToTell({ onlineRun: run }, []), run);
        assert.strictEqual(onlineRunToTell({ onlineRun: run }, [run.at]), undefined);
        assert.strictEqual(onlineRunToTell({}, []), undefined);
        assert.strictEqual(onlineRunToTell({ onlineRun: { ...run, outcome: 'other' as never } }, []), undefined);
    });
});
