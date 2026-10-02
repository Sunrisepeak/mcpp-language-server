// The cache segment's width budget, tiers and formatting (0.0.10 plan C-13.2, D12; §6).
import * as assert from 'assert';
import { cacheSegment, clampMaxLength, combineTier, fits, textWidth, CxxCacheStatus } from '../../src/cacheSegment';

const coarse = (over: Partial<CxxCacheStatus>): CxxCacheStatus => ({
    bytes: 3_800_000_000,
    limitBytes: 4_000_000_000,
    state: 'ok',
    copies: { files: 0, bytes: 0 },
    instances: { count: 0, bytes: 0 },
    ...over,
});

suite('cache segment', () => {
    test('a codicon counts as two characters, other text as one (D12)', () => {
        assert.strictEqual(textWidth('$(database) 3.8 GB'), 2 + 1 + 6);
        assert.strictEqual(textWidth('plain'), 5);
        assert.strictEqual(textWidth('$(a)$(bb)'), 4);
        assert.strictEqual(textWidth('$(unterminated'), 14);
    });

    test('the segment reflects the fill level: ok, near shows the limit, over warns (D10)', () => {
        const ok = cacheSegment(coarse({ state: 'ok' }));
        assert.ok(ok);
        assert.strictEqual(ok.tier, 0);
        assert.ok(ok.text.includes('3.80 GB'));

        const near = cacheSegment(coarse({ state: 'near' }));
        assert.ok(near);
        assert.strictEqual(near.tier, 1);
        assert.ok(near.text.includes('/'), 'near shows the budget beside the size');

        const over = cacheSegment(coarse({ state: 'over', bytes: 4_600_000_000 }));
        assert.ok(over);
        assert.strictEqual(over.tier, 2);
        assert.strictEqual(over.icon, '$(warning)');
    });

    test('no numbers, no segment', () => {
        assert.strictEqual(cacheSegment(undefined), undefined);
    });

    test('the whole item wears the worst tier of its segments, never the better one', () => {
        assert.strictEqual(combineTier(0, 2), 2);
        assert.strictEqual(combineTier(2, 1), 2, 'the cache never hides the module state');
        assert.strictEqual(combineTier(0, undefined), 0);
        assert.strictEqual(combineTier(1, 0), 1);
    });

    test('the budget holds: the module text is never dropped for the cache segment (D12)', () => {
        const settled = '$(check) C++ Modules';
        const preparing = '$(sync~spin) C++ Modules: Preparing 12/25';
        const cache = '$(database) 3.8 GB';
        assert.ok(fits(36, settled, ` ${cache}`), 'both fit the default budget when the project is settled');
        assert.ok(!fits(36, preparing, ` ${cache}`), 'a long preparing text leaves the cache segment out');
        const nearCache = '$(database) 3.80/4.00 GB';
        assert.ok(!fits(24, settled, ` ${nearCache}`), 'a 24-character budget cannot hold both');
        assert.ok(fits(24, settled), 'the module text alone fits even the smallest budget');
        assert.ok(!fits(24, preparing), 'a long preparing text needs the shortening statusText.ts does first');
    });

    test('the length budget clamps to 24..60 and defaults to 36 (D12)', () => {
        assert.strictEqual(clampMaxLength(undefined), 36);
        assert.strictEqual(clampMaxLength(12), 24);
        assert.strictEqual(clampMaxLength(80), 60);
        assert.strictEqual(clampMaxLength('48'), 48);
        assert.strictEqual(clampMaxLength('nonsense'), 36);
    });
});
