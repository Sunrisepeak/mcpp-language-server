// The drawn card v3 (2026-10-03, live review): markdown hovers give no layout control, so the
// card body is one self-drawn SVG -- centred header, one grid, right-aligned numbers, dot-matrix
// bars on one edge, theme-aware ink -- with the real links kept as markdown under it. These tests
// hold the composition to account: the x positions, the alignment anchors, the theme inks, the
// escaping, and the alt summary that shows if the image ever fails.
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import { CacheDetail, CxxCacheStatus } from '../../src/cacheSegment';
import { baseName, cardAlt, cardMarkdown, CardInput, escapeCell, escapeSvg, stateDot, svgCard } from '../../src/tooltipCard';
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
    theme: 'dark',
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

suite('tooltip card v3 (drawn)', () => {
    test('server strings cannot break the markdown or the drawing', () => {
        const escaped = escapeCell('C:\\a|b [x] `y`');
        assert.ok(!/[|`[\]]/.test(escaped.replace(/\\[|`[\]\\]/g, '')), 'every markdown metacharacter is escaped');
        assert.strictEqual(escapeCell('line1\nline2'), 'line1 line2', 'a newline cannot start a new card line');
        assert.strictEqual(escapeSvg('a<b>&"c"\'d'), 'a&lt;b&gt;&amp;&quot;c&quot;&apos;d', 'no tag of the server\'s can survive into the SVG');
    });

    test('names and dots: the last path segment, and a SHAPE per state (UI-3)', () => {
        assert.strictEqual(baseName('/work/GalTranslPP'), 'GalTranslPP');
        assert.strictEqual(baseName('D:\\work\\demo'), 'demo');
        assert.strictEqual(stateDot('ready', 'ok'), '●');
        assert.strictEqual(stateDot('degraded', 'ok'), '◐');
        assert.strictEqual(stateDot('error', 'ok'), '○');
        assert.strictEqual(stateDot('ready', 'over'), '○');
    });

    test('the header is CENTRED and carries project, state, counts and source in one line', () => {
        const svg = svgCard(input());
        const header = /<text x='200' y='22' text-anchor='middle'[^>]*>([^<]*)<\/text>/.exec(svg);
        assert.ok(header, 'a centred text element at the top');
        assert.ok(header![1].startsWith('● GalTranslPP — Ready'), header![1]);
        assert.ok(header![1].endsWith('48 modules · 176 units · mcpp'));
        const long = svgCard(input({ status: { state: 'ready', root: '/work/a-very-long-workspace-name-beyond-the-budget', source: 'mcpp' } }));
        const cut = /text-anchor='middle'[^>]*>(● [^<]*)/.exec(long)![1];
        assert.ok(cut.includes('…'), `a name that does not fit is cut, not wrapped: ${cut}`);
    });

    test('the grid: one x for every column, numbers right-anchored, bars on one edge (the layout rules)', () => {
        const svg = svgCard(input());
        const names = [...svg.matchAll(/<text x='16' y='\d+' text-anchor='start'[^>]*font-size='11'[^>]*>([^<]*)<\/text>/g)].map((match) => match[1]);
        for (const wanted of ['Published', 'Copies', 'Instances', 'Trash']) {
            assert.ok(names.includes(wanted), `${wanted} row present`);
        }
        const usedAnchors = [...svg.matchAll(/text-anchor='end'[^>]*font-size='11'/g)];
        assert.ok(usedAnchors.length >= 8, 'every class row right-aligns its size and its percent');
        const barY = [...svg.matchAll(/<rect x='224' y='(\d+)' width='12' height='8' rx='2'/g)].map((match) => match[1]);
        assert.strictEqual(new Set(barY).size, 5, 'four class bars and the budget bar, no two on one baseline');
        assert.ok(/x='384' y='\d+' text-anchor='end'[^>]*font-size='11\.5'[^>]*font-weight='600'/.test(svg), 'the total against the budget, bold, ending on the right margin');
        assert.ok(svg.includes('<line '), 'the divider before the total row');
    });

    test('the bars: twelve cells each, filled by share, monochrome ink', () => {
        const svg = svgCard(input());
        assert.strictEqual((svg.match(/rx='2'/g) ?? []).length, 5 * 12, 'five bars of twelve cells');
        assert.ok(/fill-opacity='0\.9'\/>/.test(svg), 'filled cells');
        assert.ok(/fill-opacity='0\.18'\/>/.test(svg), 'track cells');
    });

    test('the ink follows the theme, and the sweep line says its failures', () => {
        const dark = svgCard(input());
        const light = svgCard(input({ theme: 'light' }));
        assert.ok(dark.includes('#e8e8e8') && light.includes('#1f1f1f'), 'strong ink per theme');
        assert.ok(dark.includes('failed to delete'), 'failures are visible, never silent');
        assert.ok(!svgCard(input({ detail: { ...detail, lastSweep: undefined } })).includes('Last sweep'), 'no sweep, no line');
    });

    test('the preparation line appears only with real progress, and says only the truth (D18)', () => {
        const withProgress = svgCard(input({ status: { state: 'preparing', root: '/w/demo', source: 'mcpp', progress: { done: 9, total: 20 } } }));
        assert.ok(withProgress.includes('Preparing index 9/20'));
        assert.ok(withProgress.includes('45%'));
        assert.ok(!svgCard(input({ status: { state: 'preparing', root: '/w/demo', source: 'mcpp' } })).includes('Preparing index'), 'no invented numbers');
    });

    test('without a detail the card keeps its shape: the total row, coarse counts, no half-empty grid', () => {
        const svg = svgCard(input({ detail: undefined, coarse }));
        assert.ok(svg.includes('Total / budget'));
        assert.ok(svg.includes('3.80 GB / 4.00 GB'));
        assert.ok(svg.includes('instances 100 B (1)'), 'the coarse counts ride along');
        assert.ok(!svg.includes('>Published<'), 'no half-empty grid of classes');
    });

    test('the hover: the image carries the body, the real links stay markdown, the alt is the facts', () => {
        const markdown = cardMarkdown(input());
        assert.ok(markdown.startsWith('![GalTranslPP: 3.80 GB / 4.00 GB](data:image/svg+xml;utf8,'), 'the alt summary is the facts in text');
        assert.ok(markdown.includes('[$(clear-all) Sweep](command:mcppls.sweepWorkspaceCache)'));
        assert.ok(markdown.includes('[$(folder-opened) Logs](command:mcppls.revealCacheDirectory?%5B%22root%22%5D)'), 'the directory link opens the root where logs and bundles sit');
        assert.ok(markdown.includes('[$(copy) Agent prompt](command:mcppls.copyAgentPrompt)'));
        assert.ok(markdown.includes('](https://github.com/Sunrisepeak/mcpp-language-server)'), 'the repository link is a real link');
        assert.ok(markdown.includes('[$(copy)](command:mcppls.copyRepositoryUrl)'), 'the copy next to it is a command link');
        const actions = markdown.split('\n\n').find((block) => block.includes('$(clear-all)'))!;
        assert.ok(!actions.includes('\n'), 'the actions share one short line');
        void cardAlt;
    });

    test('the zh bundle translates the drawn card end to end', () => {
        const zh = JSON.parse(fs.readFileSync(path.resolve(__dirname, '..', '..', '..', 'l10n', 'bundle.l10n.zh-cn.json'), 'utf8')) as Record<string, string>;
        setLocalizer((message, ...args) => {
            const translated = zh[message] ?? message;
            return args.length > 0 ? translated.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)])) : translated;
        });
        try {
            const svg = svgCard(input());
            assert.ok(svg.includes('已发布') && svg.includes('副本拷贝') && svg.includes('实例目录') && svg.includes('垃圾箱'));
            assert.ok(svg.includes('合计 / 预算'));
            assert.ok(svg.includes('● GalTranslPP — 就绪 · 48 个模块 · 176 个单元 · mcpp'), svg.slice(0, 200));
            const markdown = cardMarkdown(input());
            assert.ok(markdown.includes('清理') && markdown.includes('Agent 提示词'));
        } finally {
            setLocalizer((message, ...args) => args.length > 0 ? message.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)])) : message);
        }
    });
});
