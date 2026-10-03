// The hub v2's items, grouping and -- the point of v2 -- behavior-carrying entries (0.0.10 plan
// C-13.3, D17, D20; 2026-10-03 UI-8..UI-10): every entry says what accepting it does, so the
// drill-down that was unreachable in v1 (nothing matched its icon test) cannot come back.
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import { CacheDetail } from '../../src/cacheSegment';
import { directoryItems, drillDownItems, entryLabel, hubItems, hubTitle, HubEntry } from '../../src/cacheHub';
import { parseSweepResult, sweepResultText } from '../../src/cacheSweep';
import { setLocalizer } from '../../src/strings';

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
    paths: { cacheRoot: 'D:/mcpplsCache/workspaces/x', logDirectory: 'D:/mcpplsCache/logs', bundlesDirectory: 'D:/mcpplsCache/bundles' },
    prompts: { agent: 'READ ONLY', issue: 'draft' },
};

suite('cache hub v2', () => {
    test('four separator groups in order, each entry opened by its codicon (UI-8, D20)', () => {
        const items = hubItems(detail, { canSweep: true });
        const separators = items.filter((item) => item.kind === 'separator').map((item) => (item as { label: string }).label);
        assert.deepStrictEqual(separators, ['Overview', 'Clean', 'Diagnostics', 'Feedback']);
        for (const item of items) {
            if (item.kind === 'entry') {
                assert.ok(/^\$\([a-z-]+\)/.test(item.icon), `the entry "${item.label}" opens with a codicon`);
            }
        }
    });

    test('every entry carries its behavior: the data contract that replaced icon parsing (UI-9)', () => {
        for (const item of hubItems(detail, { canSweep: true })) {
            if (item.kind !== 'entry') continue;
            const entry = item as { behavior: HubEntry['behavior'] };
            assert.ok(['sweep', 'command', 'detail', 'refresh'].includes(entry.behavior), `"${item.label}" says what accepting it does`);
            if (entry.behavior === 'command') {
                assert.ok(item.action !== undefined, `"${item.label}" names the command it runs`);
            }
            if (entry.behavior === 'detail') {
                assert.ok(item.detail === 'modules' || item.detail === 'directories', `"${item.label}" names the drill-down list`);
            }
        }
    });

    test('the drill-down entry EXISTS and is reachable data: the v1 dead branch cannot return (UI-9)', () => {
        const items = hubItems(detail, { canSweep: true });
        const entry = items.find((item) => item.kind === 'entry' && item.detail === 'modules');
        assert.ok(entry, 'an entry whose behavior is the modules drill-down');
        const directories = items.find((item) => item.kind === 'entry' && item.detail === 'directories');
        assert.ok(directories, 'an entry whose behavior is the directories drill-down');
    });

    test('the sweep stays the first action of the Clean group; an old server loses only it (D15)', () => {
        const items = hubItems(detail, { canSweep: true });
        const clean = items.findIndex((item) => item.kind === 'separator' && item.label === 'Clean');
        const firstAction = items[clean + 1];
        assert.ok(firstAction.kind === 'entry' && firstAction.behavior === 'sweep');
        assert.ok(firstAction.buttonTitle !== undefined, 'the dry-run eye hangs on the sweep entry');
        const without = hubItems(detail, { canSweep: false });
        assert.ok(!without.some((item) => item.kind === 'entry' && item.behavior === 'sweep'));
        assert.ok(without.some((item) => item.kind === 'entry' && item.label === 'Reset the cache…'));
    });

    test('no engine name anywhere in the hub (D17), and the title carries the cache against its budget', () => {
        assert.ok(!JSON.stringify(hubItems(detail, { canSweep: true })).includes('clangd'));
        assert.strictEqual(hubTitle(detail), 'GalTranslPP — 3.80 GB / 4.00 GB · 95%');
    });

    test('the drill-downs: largest modules read-only, three exact directories (D20, UI-8)', () => {
        const modules = drillDownItems(detail).map((item) => (item.kind === 'entry' ? `${entryLabel(item)} ${item.description ?? ''}` : item.label)).join('\n');
        assert.ok(modules.includes('pybind11.ixx'));
        assert.ok(modules.includes('127 copies'));
        assert.ok(modules.includes('Copy the issue draft prompt'));
        const directories = directoryItems(detail);
        const targets = directories.filter((item) => item.kind === 'entry').map((item) => (item as { action: { arguments: string[] } }).action.arguments[0]);
        assert.deepStrictEqual(targets, ['cache', 'logs', 'bundles']);
    });

    test('sweep results say what happened, including that nothing restarts', () => {
        assert.ok(sweepResultText(parseSweepResult({ ok: true, freedBytes: 1_288_490_188_288, files: 6837, roots: 1, dryRun: false })).includes('No restart'));
        assert.ok(sweepResultText(parseSweepResult({ ok: true, freedBytes: 1_288_490_188_288, files: 6837, roots: 1, dryRun: false })).includes('1.29 TB'),
            'the receipt reads like a person reads sizes, not raw bytes');
        assert.ok(sweepResultText(parseSweepResult({ ok: true, freedBytes: 0, files: 0, roots: 1, dryRun: false })).includes('already'));
        assert.ok(sweepResultText(parseSweepResult({ ok: true, freedBytes: 100, files: 1, roots: 1, dryRun: true })).includes('would'));
        assert.ok(sweepResultText(parseSweepResult({ ok: true, alreadyRunning: true })).includes('already running'));
        const nonsense = parseSweepResult('not an object');
        assert.strictEqual(nonsense.freedBytes, 0);
        assert.strictEqual(nonsense.ok, false);
    });

    test('the zh bundle translates the hub: the four groups and the primary action', () => {
        const zh = JSON.parse(fs.readFileSync(path.resolve(__dirname, '..', '..', '..', 'l10n', 'bundle.l10n.zh-cn.json'), 'utf8')) as Record<string, string>;
        setLocalizer((message, ...args) => {
            const translated = zh[message] ?? message;
            return args.length > 0 ? translated.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)])) : translated;
        });
        try {
            const separators = hubItems(detail, { canSweep: true })
                .filter((item) => item.kind === 'separator')
                .map((item) => (item as { label: string }).label);
            assert.deepStrictEqual(separators, ['概览', '清理', '诊断', '反馈']);
            const items = hubItems(detail, { canSweep: true });
            const sweep = items.find((item) => item.kind === 'entry' && item.behavior === 'sweep');
            assert.ok(sweep && sweep.label.includes('清理缓存（不重启、不重编）'));
        } finally {
            setLocalizer((message, ...args) => args.length > 0 ? message.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)])) : message);
        }
    });
});
