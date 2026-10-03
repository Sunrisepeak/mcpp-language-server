// The hover card's markdown v2 (0.0.10 plan C-13.2; 2026-10-03 UI-2..UI-7): the three zones, the
// per-class bars, the budget headline, the three common actions and the repository line. The
// default localizer is the source language, so these assertions read English; one test switches to
// the real zh bundle to prove the card renders translated.
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import { CacheDetail, CxxCacheStatus } from '../../src/cacheSegment';
import { bar, baseName, cardMarkdown, CardInput, escapeCell, stateDot } from '../../src/tooltipCard';
import { setLocalizer } from '../../src/strings';

const detail: CacheDetail = {
    state: 'ready',
    project: { name: 'GalTranslPP', source: 'mcpp' },
    plan: { units: 176, modules: 48 },
    bytes: 3_800_000_000,
    canonical: { files: 48, bytes: 1_900_000_000 },
    copies: { files: 6837, bytes: 1_700_000_000, oldestSeconds: 90 },
    trash: { bytes: 1_000 },
    instances: { count: 1, bytes: 100_000_000, list: [] },
    limits: { perWorkspace: 4_000_000_000, total: 16_000_000_000, over: false },
    lastSweep: { at: Date.now() - 30_000, freedBytes: 1_200_000_000, files: 6837, failed: 2 },
    paths: { cacheRoot: 'C:/a|b', logDirectory: 'D:\\mcpplsCache\\logs', bundlesDirectory: 'D:\\mcpplsCache\\bundles' },
} as unknown as CacheDetail;

const input = (over: Partial<CardInput> = {}): CardInput => ({
    status: { state: 'ready', root: '/work/GalTranslPP', source: 'mcpp' },
    detail,
    withCommands: true,
    sweepCommand: 'mcppls.sweepWorkspaceCache',
    revealCommand: 'mcppls.revealCacheDirectory',
    copyPromptCommand: 'mcppls.copyAgentPrompt',
    copyRepositoryCommand: 'mcppls.copyRepositoryUrl',
    ...over,
});

const coarse: CxxCacheStatus = {
    bytes: 3_800_000_000,
    limitBytes: 4_000_000_000,
    state: 'ok',
    copies: { files: 3, bytes: 300 },
    instances: { count: 1, bytes: 100 },
    lastSweep: { at: Date.now(), freedBytes: 5_000_000 },
};

suite('tooltip card v2', () => {
    test('server strings cannot break the markdown structure', () => {
        const escaped = escapeCell('C:\\a|b [x] `y`');
        assert.ok(!/[|`[\]]/.test(escaped.replace(/\\[|`[\]\\]/g, '')), 'every metacharacter is escaped');
        assert.strictEqual(escapeCell('line1\nline2'), 'line1 line2', 'a newline cannot start a new card line');
    });

    test('the bar is one monochrome dot-matrix language, fixed width (v2.4)', () => {
        assert.strictEqual(bar(0.5), '`██████░░░░░░`');
        assert.strictEqual(bar(0), '`░░░░░░░░░░░░`');
        assert.strictEqual(bar(1), '`████████████`');
        assert.strictEqual(bar(2), '`████████████`', 'out-of-range shares clamp, never overflow');
        assert.strictEqual(bar(Number.NaN), '`░░░░░░░░░░░░`');
    });

    test('names and dots: the last path segment, and a SHAPE per state (UI-3)', () => {
        assert.strictEqual(baseName('/work/GalTranslPP'), 'GalTranslPP');
        assert.strictEqual(baseName('D:\\work\\demo'), 'demo');
        assert.strictEqual(stateDot('ready', 'ok'), '●');
        assert.strictEqual(stateDot('degraded', 'ok'), '◐');
        assert.strictEqual(stateDot('error', 'ok'), '○');
        assert.strictEqual(stateDot('ready', 'over'), '○');
    });

    test('three zones: project first, the one cache table second, actions and repository last', () => {
        const markdown = cardMarkdown(input());
        assert.ok(markdown.startsWith('● **GalTranslPP — Ready** · 48 modules · 176 units · mcpp'), markdown.split('\n')[0]);
        assert.ok(markdown.includes('| Class | Used | Share |  |'));
        assert.ok(markdown.includes('| Published | 1.90 GB | 50% | `██████░░░░░░` |'));
        assert.ok(markdown.includes('| Copies | 1.70 GB | 45% | `█████░░░░░░░` |'));
        assert.ok(markdown.includes('| Instances | 100 MB | 3% |'), 'a class below an eighth of a cell keeps its row, at zero cells');
        assert.ok(markdown.includes('| Trash | 1.00 KB | 0% |'), 'a class that exists still gets its row');
        assert.ok(markdown.includes('| **Total / budget** | **3.80 GB / 4.00 GB** | **95%** | `███████████░` |'), 'the total row IS the headline, inside the grid, with its budget bar');
        assert.ok(markdown.includes('failed to delete'), 'failures are visible, never silent');
    });

    test('every row is its own line: zones are blank-line separated, rows hard-broken (the v1 run-on fix)', () => {
        const zones = cardMarkdown(input()).split('\n\n');
        assert.ok(zones.length >= 4, `${zones.length} zones: ${zones.map((z) => z.split('\n')[0]).join(' | ')}`);
        assert.ok(!zones[0].includes('\n'), 'the project zone is ONE line: dot, name, state, counts, source');
        const table = zones.find((zone) => zone.startsWith('| Class |'));
        assert.ok(table !== undefined, 'the table is a block of its own');
        assert.ok(table!.split('\n').length === 7, 'header, ruler, four classes, the total row');
    });

    test('the preparation line appears only with real progress, and says only the truth (D18)', () => {
        const withProgress = cardMarkdown(input({ status: { state: 'preparing', root: '/w/demo', source: 'mcpp', progress: { done: 9, total: 20 } } }));
        assert.ok(withProgress.includes('Preparing index 9/20'));
        assert.ok(withProgress.includes('45%'));
        const withoutProgress = cardMarkdown(input({ status: { state: 'preparing', root: '/w/demo', source: 'mcpp' }, detail: { ...detail, progress: undefined } }));
        assert.ok(!withoutProgress.includes('Preparing index'), 'no invented numbers');
    });

    test('the actions are the three most common, and the repository line replaces the footnote (UI-5, UI-6)', () => {
        const markdown = cardMarkdown(input());
        assert.ok(markdown.includes('[$(clear-all) Sweep cache](command:mcppls.sweepWorkspaceCache)'));
        assert.ok(markdown.includes('[$(folder-opened) Open logs & reports](command:mcppls.revealCacheDirectory?%5B%22root%22%5D)'), 'the directory link opens the root where logs and bundles sit');
        assert.ok(markdown.includes('[$(copy) Copy agent prompt](command:mcppls.copyAgentPrompt)'), 'the prompt link says what it does: copy, for an agent');
        assert.ok(markdown.includes('](https://github.com/Sunrisepeak/mcpp-language-server)'), 'the repository link is a real link');
        assert.ok(markdown.includes('[$(copy)](command:mcppls.copyRepositoryUrl)'), 'the copy next to it is a command link');
        assert.ok(!markdown.includes('never leaves this machine'), 'the old footnote is gone');
    });

    test('the card stays within thirteen rendered lines, and a long project name is cut (plan §6)', () => {
        const rendered = cardMarkdown(input()).split('\n').filter((line) => line.trim().length > 0);
        assert.ok(rendered.length <= 13, `${rendered.length} lines: ${rendered.join(' / ')}`);
        const long = cardMarkdown(input({ status: { state: 'ready', root: '/work/' + 'a-very-long-workspace-name-beyond-the-budget', source: 'mcpp' } }));
        assert.ok(long.includes('…'), 'a name that does not fit is cut, not wrapped');
    });

    test('without a detail the card falls back to the coarse numbers, with the budget row as its one chart', () => {
        const markdown = cardMarkdown(input({ detail: undefined, coarse }));
        assert.ok(markdown.includes('| **3.80 GB / 4.00 GB** | 95% | `███████████░` |'), 'the total row renders from the coarse numbers alone');
        assert.ok(markdown.includes('Copies 300 B (3 files) · instances 100 B (1)'));
        assert.ok(!markdown.includes('| Class |'), 'no half-empty table');
    });

    test('the zh bundle translates the card end to end', () => {
        const zh = JSON.parse(fs.readFileSync(path.resolve(__dirname, '..', '..', '..', 'l10n', 'bundle.l10n.zh-cn.json'), 'utf8')) as Record<string, string>;
        setLocalizer((message, ...args) => {
            const translated = zh[message] ?? message;
            return args.length > 0 ? translated.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)])) : translated;
        });
        try {
            const markdown = cardMarkdown(input());
            assert.ok(markdown.startsWith('● **GalTranslPP — 就绪** · 48 个模块 · 176 个单元 · mcpp'), markdown.split('\n')[0]);
            assert.ok(markdown.includes('| **合计 / 预算** | **3.80 GB / 4.00 GB** | **95%** |'));
            assert.ok(markdown.includes('| 已发布 | 1.90 GB | 50% | `██████░░░░░░` |'));
            assert.ok(markdown.includes('| 副本拷贝 | 1.70 GB | 45% | `█████░░░░░░░` |'));
            assert.ok(markdown.includes('清理缓存'));
            assert.ok(markdown.includes('复制 Agent 提示词'));
        } finally {
            setLocalizer((message, ...args) => args.length > 0 ? message.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)])) : message);
        }
    });
});
