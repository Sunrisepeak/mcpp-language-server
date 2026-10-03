// The hover card v3.1 (2026-10-03, live review): markdown all the way down -- the dot-matrix
// table of v2.4 plus a footer of exactly two lines that ALIGN with each other (the narrower one
// padded with no-break spaces, half the width difference, CJK counted at two columns). The tests
// hold the table's shape, the one-line header, the footer's alignment arithmetic and the zh
// rendering to account.
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import { CacheDetail, CxxCacheStatus } from '../../src/cacheSegment';
import { alignedPair, bar, baseName, cardMarkdown, CardInput, escapeCell, stateDot, visibleWidth } from '../../src/tooltipCard';
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

suite('tooltip card v3.1 (markdown, aligned footer)', () => {
    test('server strings cannot break the markdown structure', () => {
        const escaped = escapeCell('C:\\a|b [x] `y`');
        assert.ok(!/[|`[\]]/.test(escaped.replace(/\\[|`[\]\\]/g, '')), 'every metacharacter is escaped');
        assert.strictEqual(escapeCell('line1\nline2'), 'line1 line2', 'a newline cannot start a new card line');
    });

    test('the bar is one monochrome dot-matrix language, fixed width', () => {
        assert.strictEqual(bar(0.5), '`███████░░░░░░`');
        assert.strictEqual(bar(0), '`░░░░░░░░░░░░░`');
        assert.strictEqual(bar(1), '`█████████████`');
        assert.strictEqual(bar(2), '`█████████████`', 'out-of-range shares clamp, never overflow');
        assert.strictEqual(bar(Number.NaN), '`░░░░░░░░░░░░░`');
    });

    test('widths the way the hover lays them out: icons two, CJK two, latin one', () => {
        assert.strictEqual(visibleWidth('abc'), 3);
        assert.strictEqual(visibleWidth('$(clear-all)'), 2);
        assert.strictEqual(visibleWidth('$(clear-all) 清理'), 2 + 1 + 4, 'the zh pair of characters counts four');
        assert.strictEqual(visibleWidth('缓存'), 4);
    });

    test('names and dots: the last path segment, and a SHAPE per state (UI-3)', () => {
        assert.strictEqual(baseName('/work/GalTranslPP'), 'GalTranslPP');
        assert.strictEqual(baseName('D:\\work\\demo'), 'demo');
        assert.strictEqual(stateDot('ready', 'ok'), '●');
        assert.strictEqual(stateDot('degraded', 'ok'), '◐');
        assert.strictEqual(stateDot('error', 'ok'), '○');
        assert.strictEqual(stateDot('ready', 'over'), '○');
    });

    test('one title line: dot, name, state, counts, source; long names cut, not wrapped', () => {
        const markdown = cardMarkdown(input());
        assert.ok(markdown.startsWith('● **GalTranslPP — Ready** · 48 modules · 176 units · mcpp'), markdown.split('\n')[0]);
        const long = cardMarkdown(input({ status: { state: 'ready', root: '/work/a-very-long-workspace-name-beyond-the-budget', source: 'mcpp' } }));
        assert.ok(long.split('\n')[0].includes('…'), 'a name that does not fit is cut');
    });

    test('the one cache table: all four classes, code-span bars, the bold total row', () => {
        const markdown = cardMarkdown(input());
        assert.ok(markdown.includes('| Class | Used | Share |  |'));
        assert.ok(markdown.includes('| Published | 1.90 GB | 50% | `███████░░░░░░` |'));
        assert.ok(markdown.includes('| Copies | 1.70 GB | 45% | `██████░░░░░░░` |'));
        assert.ok(markdown.includes('| Instances | 100 MB | 3% |'), 'a class below a cell keeps its row');
        assert.ok(markdown.includes('| Trash | 1.00 KB | 0% |'));
        assert.ok(markdown.includes('| **Total** | **3.80 GB / 4.00 GB** | **95%** | `████████████░` |'));
        assert.ok(markdown.includes('failed to delete'), 'failures are visible, never silent');
        const zones = markdown.split('\n\n');
        assert.ok(zones[0].includes('GalTranslPP') && !zones[0].includes('  \n'), 'the title is ONE line');
        const table = zones.find((block) => block.startsWith('| Class |'))!;
        assert.strictEqual(table.split('\n').length, 7, 'header, ruler, four classes, the total row');
    });

    test('the preparation line appears only with real progress (D18)', () => {
        const withProgress = cardMarkdown(input({ status: { state: 'preparing', root: '/w/demo', source: 'mcpp', progress: { done: 9, total: 20 } } }));
        assert.ok(withProgress.includes('Preparing index 9/20'));
        assert.ok(withProgress.includes('45%'));
        assert.ok(!cardMarkdown(input({ status: { state: 'preparing', root: '/w/demo', source: 'mcpp' } })).includes('Preparing index'));
    });

    test('without a detail the card keeps its shape: the total row, coarse counts, no half-empty grid', () => {
        const markdown = cardMarkdown(input({ detail: undefined, coarse }));
        assert.ok(markdown.includes('| **3.80 GB / 4.00 GB** | 95% | `███████████████████░` |'));
        assert.ok(markdown.includes('Copies 300 B (3 files) · instances 100 B (1)'));
        assert.ok(!markdown.includes('| Class |'), 'no half-empty table');
    });

    test('the footer: exactly two lines, aligned with each other by no-break spaces', () => {
        const markdown = cardMarkdown(input());
        assert.ok(!markdown.includes('data:image'), 'no drawn image anywhere');
        const footer = markdown.split('\n\n').pop()!;
        const lines = footer.split('  \n');
        assert.strictEqual(lines.length, 2, 'the actions and the repository, two lines');
        assert.ok(lines[0].includes('[$(clear-all) Sweep cache](command:mcppls.sweepWorkspaceCache)'));
        assert.ok(lines[0].includes('[$(folder-opened) Open logs](command:mcppls.revealCacheDirectory?%5B%22logs%22%5D)'),
            'the label says logs, so the link opens the logs directory itself');
        assert.ok(lines[0].includes('[$(copy) Copy agent prompt](command:mcppls.copyAgentPrompt)'));
        assert.ok(lines[1].includes('](https://github.com/Sunrisepeak/mcpp-language-server)'), 'the repository link is a real link');
        assert.ok(lines[1].includes('[$(copy)](command:mcppls.copyRepositoryUrl)'), 'the copy next to it is a command link');
        // the alignment arithmetic: the narrower line is padded by half the difference
        const pad = (line: string): number => line.length - line.replace(/^\u00A0+/, '').length;
        assert.ok(Math.max(...lines.map(pad)) <= 2, 'no visible run of padding spaces: the names carry the balance');
    });

    test('alignedPair centers the pair by half the width difference, in either script', () => {
        const pair = alignedPair(
            { text: '[short]', plain: '清理' },                      // width 4
            { text: '[longer]', plain: 'github.com/x' },             // width 12
        );
        const [first, second] = pair.split('  \n');
        assert.strictEqual(first.match(/^\u00A0+/)![0].length, 4, 'floor((12-4)/2) no-break spaces');
        assert.ok(!second.startsWith('\u00A0'), 'the wider line starts at the edge');
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
            assert.ok(markdown.includes('| 已发布 | 1.90 GB | 50% | `████████░░░░░░░` |'));
            assert.ok(markdown.includes('| **合计** | **3.80 GB / 4.00 GB** |'));
            assert.ok(markdown.includes('清理缓存') && markdown.includes('复制 Agent 提示词'));
        } finally {
            setLocalizer((message, ...args) => args.length > 0 ? message.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)])) : message);
        }
    });

    test('the bar column adapts per language: en 13, zh longer, both landing the same band', () => {
        // The review's ask (2026-10-04): the bars carry their share of the table's width, en at 13
        // cells, and the narrower zh words leave more of the band -- so the two languages' tables
        // close on the same right edge instead of the zh one reading narrower.
        const en = cardMarkdown(input());
        const cellsOf = (markdown: string, row: RegExp): number => markdown.match(row)![1].length;
        assert.strictEqual(cellsOf(en, /\| Published[^\n]*`([█░]+)`/), 13, 'the en table, the width the person picked');
        const widest = (markdown: string): number => Math.max(...markdown.split('\n\n').find((block) => block.startsWith('|'))!
            .split('\n').map((row) => visibleWidth(row.replace(/\*\*|`/g, ''))));
        const zh = JSON.parse(fs.readFileSync(path.resolve(__dirname, '..', '..', '..', 'l10n', 'bundle.l10n.zh-cn.json'), 'utf8')) as Record<string, string>;
        setLocalizer((message, ...args) => {
            const translated = zh[message] ?? message;
            return args.length > 0 ? translated.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)])) : translated;
        });
        try {
            const zhMarkdown = cardMarkdown(input());
            assert.ok(cellsOf(zhMarkdown, /\| 已发布[^\n]*`([█░]+)`/) > 13, 'the zh bars run longer for the same band');
            assert.ok(Math.abs(widest(zhMarkdown) - widest(en)) <= 1, `both tables land on one width band: zh ${widest(zhMarkdown)} en ${widest(en)}`);
        } finally {
            setLocalizer((message, ...args) => args.length > 0 ? message.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)])) : message);
        }
    });
});
