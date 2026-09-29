// Which issues produce the notification, with which actions, and once.
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import { SERVER_RESET_CACHE_COMMAND } from '../../src/cacheReset';
import { ACTION_LABELS, BUNDLE_CODES, BUNDLE_DETAIL, FATAL_CODES, NoticeLedger, noticeFor, noticeText } from '../../src/unrecoverable';

const BUNDLE = '/home/x/.cache/mcppls/bundles/auto-1.zip';

suite('unrecoverable errors', () => {
    test('an issue with a bundle is a notice with the five actions', () => {
        const notice = noticeFor({ code: 'engine-crash-loop', message: 'clangd keeps crashing', bundle: BUNDLE });
        assert.ok(notice);
        assert.strictEqual(notice.bundle, BUNDLE);
        assert.deepStrictEqual(notice.actions.map((action) => ACTION_LABELS[action]), [
            'Report Issue…', 'Restart Server', 'Reset This Workspace\'s Cache', 'Turn Off in This Workspace', 'Show Logs']);
        assert.strictEqual(noticeText(notice), `C++ Modules: clangd keeps crashing ${BUNDLE_DETAIL}`);
        assert.ok(BUNDLE_DETAIL.includes('stays on this machine'));
    });

    test('any code with a bundle counts; a fatal code without one counts for an older server', () => {
        assert.ok(noticeFor({ code: 'something-new', message: 'm', bundle: BUNDLE }));
        for (const code of FATAL_CODES) {
            const notice = noticeFor({ code, message: 'm' });
            assert.ok(notice, code);
            assert.strictEqual(notice.bundle, undefined);
        }
        for (const code of BUNDLE_CODES) assert.ok(noticeFor({ code, message: 'm', bundle: BUNDLE }), code);
    });

    test('everything else is left alone', () => {
        assert.strictEqual(noticeFor({ code: 'module-lock-stale', message: 'm' }), undefined);
        assert.strictEqual(noticeFor({ code: 'preparation-stalled', message: 'm' }), undefined);
        assert.strictEqual(noticeFor({ code: 'unresolved-module', message: 'm', category: 'code' }), undefined);
        assert.strictEqual(noticeFor({ code: 'engine-crash-loop', message: 'm', category: 'code', bundle: BUNDLE }), undefined);
    });

    test('once per code per session, and again when a bundle arrives later', () => {
        const ledger = new NoticeLedger();
        const first = ledger.take([{ code: 'engine-crash-loop', message: 'm', bundle: BUNDLE }, { code: 'other', message: 'o' }]);
        assert.deepStrictEqual(first.map((notice) => notice.code), ['engine-crash-loop']);
        assert.deepStrictEqual(ledger.take([{ code: 'engine-crash-loop', message: 'm', bundle: BUNDLE }]), []);
        assert.deepStrictEqual(ledger.take(undefined), []);
        assert.strictEqual(ledger.take([{ code: 'engine-start-failed', message: 'm' }]).length, 1);
        assert.deepStrictEqual(ledger.take([{ code: 'engine-start-failed', message: 'm' }]), []);
        assert.strictEqual(ledger.take([{ code: 'engine-start-failed', message: 'm', bundle: BUNDLE }]).length, 1);
        assert.deepStrictEqual(ledger.take([{ code: 'engine-start-failed', message: 'm', bundle: BUNDLE }]), []);
    });

    // vscode-languageclient registers every command the server declares, so an id the extension
    // registers itself and the server also declares stops the client from starting.
    test('the extension registers none of the server\'s command ids', () => {
        const server = ['mcppls.exportBundle', 'mcppls.restartEngine', 'mcppls.resetCache', 'mcppls.reloadBuildDescription',
            'mcppls.describeOnline', 'mcppls.review.run', 'mcppls.review.clear'];
        const sources = ['commands.ts', 'enable.ts', 'fatal.ts', 'extension.ts'].map((name) =>
            fs.readFileSync(path.resolve(__dirname, '..', '..', '..', 'src', name), 'utf8')).join('\n');
        const registered = [...sources.matchAll(/registerCommand\(\s*'([^']+)'/g)].map((match) => match[1]);
        assert.ok(registered.length > 5);
        for (const id of server) assert.ok(!registered.includes(id), id);
        assert.ok(server.includes(SERVER_RESET_CACHE_COMMAND));
    });
});
