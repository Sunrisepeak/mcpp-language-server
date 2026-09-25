// The status wording rules (design 2026-09-25 §6, S3 extension `category`), in plain Node: no VS
// Code, same reason test/unit/serverLog.test.ts is.
import * as assert from 'assert';
import { firstNonCodeIssue, isCodeIssue, shorten, stateTexts, StatusIssue } from '../../src/statusText';

suite('status issue categories', () => {
    test('an issue with no category is treated as non-code (older servers)', () => {
        const issue: StatusIssue = { code: 'engine-timeout', message: 'clangd stopped responding' };
        assert.strictEqual(isCodeIssue(issue), false);
        assert.strictEqual(firstNonCodeIssue([issue]), issue);
    });

    test('category "code" is excluded from the non-code search', () => {
        const codeIssue: StatusIssue = { code: 'unresolved-module', message: 'import helo; not found', category: 'code' };
        const engineIssue: StatusIssue = { code: 'engine-timeout', message: 'clangd stopped responding', category: 'engine' };
        assert.strictEqual(firstNonCodeIssue([codeIssue, engineIssue]), engineIssue);
        assert.strictEqual(firstNonCodeIssue([codeIssue]), undefined);
    });

    test('every other category counts as non-code', () => {
        for (const category of ['engine', 'environment', 'project'] as const) {
            const issue: StatusIssue = { code: 'x', message: 'm', category };
            assert.strictEqual(isCodeIssue(issue), false);
        }
    });
});

suite('shorten', () => {
    test('leaves a short message alone', () => {
        assert.strictEqual(shorten('clangd stopped responding'), 'clangd stopped responding');
    });

    test('cuts at the first sentence when that already fits', () => {
        assert.strictEqual(
            shorten('main.cpp: basic features only. clangd stopped responding and was restarted.'),
            'main.cpp: basic features only.',
        );
    });

    test('falls back to a word-boundary truncation for one long sentence', () => {
        const long = 'a'.repeat(30) + ' ' + 'b'.repeat(60);
        const result = shorten(long, 40);
        assert.ok(result.length <= 40, `expected <= 40 chars, got ${result.length}: ${result}`);
        assert.ok(result.endsWith('…'), `expected an ellipsis, got: ${result}`);
        assert.ok(!result.includes('bbbbb'), 'must not cut mid-word into the long run of b');
    });
});

suite('status text: state, category and wording rules', () => {
    test('the generic "Some features are limited" phrase is gone', () => {
        const texts = stateTexts({ state: 'degraded', issues: [{ code: 'engine-timeout', message: 'clangd stopped responding' }] });
        assert.notStrictEqual(texts.short, 'Some features are limited');
        assert.notStrictEqual(texts.full, 'Some features are limited');
    });

    test('degraded shows the first non-code issue, shortened for the bar, in full for the tooltip', () => {
        const long = 'main.cpp: basic features only. clangd stopped responding and was restarted after a 20 second stall.';
        const texts = stateTexts({ state: 'degraded', issues: [{ code: 'engine-timeout', message: long }] });
        assert.strictEqual(texts.short, shorten(long));
        assert.strictEqual(texts.full, long);
    });

    test('degraded skips a code issue and uses the first non-code one instead', () => {
        const texts = stateTexts({
            state: 'degraded',
            issues: [
                { code: 'unresolved-module', message: 'import helo; not found', category: 'code' },
                { code: 'engine-timeout', message: 'clangd stopped responding', category: 'engine' },
            ],
        });
        assert.strictEqual(texts.full, 'clangd stopped responding');
    });

    test('degraded with only a code issue (a defensive fallback, not expected in practice) does not crash', () => {
        const texts = stateTexts({
            state: 'degraded',
            issues: [{ code: 'unresolved-module', message: 'import helo; not found', category: 'code' }],
        });
        assert.strictEqual(texts.full, 'Limited');
        assert.notStrictEqual(texts.full, 'Some features are limited');
    });

    test('error keeps its sentence and appends why', () => {
        const texts = stateTexts({ state: 'error', issues: [{ code: 'engine-crashed', message: 'clangd exited repeatedly' }] });
        assert.strictEqual(texts.full, 'Only module-level features are available: clangd exited repeatedly');
        assert.strictEqual(texts.short, texts.full);
    });

    test('error with no issues keeps the sentence alone', () => {
        const texts = stateTexts({ state: 'error', issues: [] });
        assert.strictEqual(texts.full, 'Only module-level features are available');
    });

    test('error names why from the first non-code issue, skipping a code one', () => {
        const texts = stateTexts({
            state: 'error',
            issues: [
                { code: 'unresolved-module', message: 'import helo; not found', category: 'code' },
                { code: 'engine-crashed', message: 'clangd exited repeatedly', category: 'engine' },
            ],
        });
        assert.strictEqual(texts.full, 'Only module-level features are available: clangd exited repeatedly');
    });

    test('ready has no text', () => {
        assert.deepStrictEqual(stateTexts({ state: 'ready' }), { short: undefined, full: undefined });
    });

    test('preparing shows progress, and it is the same in short and full form', () => {
        const texts = stateTexts({ state: 'preparing', progress: { done: 9, total: 646 } });
        assert.deepStrictEqual(texts, { short: 'Preparing modules 9/646', full: 'Preparing modules 9/646' });
    });
});
