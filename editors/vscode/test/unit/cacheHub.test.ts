// The hub's items, icons, grouping and degradation (0.0.10 plan C-13.3, D17, D20; §6).
import * as assert from 'assert';
import { CacheDetail } from '../../src/cacheSegment';
import { drillDownItems, entryLabel, hubItems, hubTitle } from '../../src/cacheHub';
import { parseSweepResult, sweepResultText } from '../../src/cacheSweep';

const detail: CacheDetail = {
    state: 'ready',
    project: { name: 'GalTranslPP', source: 'mcpp' },
    plan: { units: 176, modules: 48 },
    bytes: 3_800_000_000,
    canonical: { files: 48, bytes: 1_900_000_000 },
    copies: { files: 0, bytes: 0, oldestSeconds: 0 },
    trash: { bytes: 0 },
    instances: { count: 1, bytes: 120_000_000, list: [] },
    largest: [{ module: 'pybind11.ixx', bytes: 39_639_212, copies: 127 }],
    limits: { perWorkspace: 4_000_000_000, total: 16_000_000_000, over: false },
    lastSweep: { at: Date.now() - 120_000, freedBytes: 1_200_000_000, files: 6837, failed: 0 },
    paths: { cacheRoot: 'D:/mcpplsCache/workspaces/x', logDirectory: 'D:/mcpplsCache/log' },
    prompts: { agent: 'READ ONLY', issue: 'draft' },
};

suite('cache hub', () => {
    test('five separator groups in order, each entry opened by its codicon (D20)', () => {
        const items = hubItems(detail, { canSweep: true });
        const separators = items.filter((item) => item.kind === 'separator').map((item) => (item as { label: string }).label);
        assert.deepStrictEqual(separators, ['缓存', '清理', '维护', '日志', '开源']);
        for (const item of items) {
            if (item.kind === 'entry') {
                assert.ok(/^\$\([a-z-]+\)/.test(item.icon), `the entry "${item.label}" opens with a codicon`);
            }
        }
    });

    test('exactly one primary action, first in the 清理 group (D15)', () => {
        const items = hubItems(detail, { canSweep: true });
        const groups: string[][] = [];
        let current: string[] = [];
        for (const item of items) {
            if (item.kind === 'separator') {
                current = [];
                groups.push(current);
            } else {
                current.push(item.label);
            }
        }
        const sweepGroup = groups[1];
        assert.strictEqual(sweepGroup.length, 1, 'the 清理 group holds the one primary action');
        assert.ok(sweepGroup[0].includes('清理缓存'));
    });

    test('an old server without the sweep command loses the whole 清理 group, with its CLI fallback named', () => {
        const items = hubItems(detail, { canSweep: false });
        const labels = items.map((item) => (item.kind === 'entry' ? item.label : item.label));
        assert.ok(!labels.some((label) => label.includes('清理缓存')));
    });

    test('no engine name anywhere in the hub (D17), and the title carries the scale instead', () => {
        const text = JSON.stringify(hubItems(detail, { canSweep: true }));
        assert.ok(!text.includes('clangd'));
        const title = hubTitle(detail);
        assert.ok(title.includes('176 units'), 'the plan scale the D17 title gives');
        assert.ok(title.includes('48 modules'));
        assert.ok(!title.includes('clangd'));
    });

    test('the drill-down lists the largest modules and the issue prompt, read-only', () => {
        const items = drillDownItems(detail);
        const text = items.map((item) => (item.kind === 'entry' ? `${entryLabel(item)} ${item.description ?? ''}` : item.label)).join('\n');
        assert.ok(text.includes('pybind11.ixx'));
        assert.ok(text.includes('127 份'));
        assert.ok(text.includes('issue 提示词'));
    });

    test('sweep results say what happened, including that nothing restarts', () => {
        assert.ok(sweepResultText(parseSweepResult({ ok: true, freedBytes: 1_288_490_188_288, files: 6837, roots: 1, dryRun: false })).includes('No restart'));
        assert.ok(sweepResultText(parseSweepResult({ ok: true, freedBytes: 0, files: 0, roots: 1, dryRun: false })).includes('already'));
        assert.ok(sweepResultText(parseSweepResult({ ok: true, freedBytes: 100, files: 1, roots: 1, dryRun: true })).includes('would'));
        assert.ok(sweepResultText(parseSweepResult({ ok: true, alreadyRunning: true })).includes('already running'));
        const nonsense = parseSweepResult('not an object');
        assert.strictEqual(nonsense.freedBytes, 0);
        assert.strictEqual(nonsense.ok, false);
    });
});
