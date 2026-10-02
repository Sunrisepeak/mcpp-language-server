// The hover card's markdown: escaping, the bar, the four classes (0.0.10 plan C-13.2/C-13.3; §6).
import * as assert from 'assert';
import { CacheDetail } from '../../src/cacheSegment';
import { cardMarkdown, cacheCardLines, distributionBar, escapeCell } from '../../src/tooltipCard';

const detail: CacheDetail = {
    state: 'ready',
    project: { name: 'GalTranslPP', source: 'mcpp' },
    bytes: 3_800_000_000,
    canonical: { files: 48, bytes: 1_900_000_000 },
    copies: { files: 6837, bytes: 1_700_000_000, oldestSeconds: 90 },
    trash: { bytes: 1_000 },
    instances: { count: 1, bytes: 100_000_000, list: [] },
    limits: { perWorkspace: 4_000_000_000, total: 16_000_000_000, over: false },
    copies2: undefined,
    lastSweep: { at: Date.now() - 30_000, freedBytes: 1_200_000_000, files: 6837, failed: 2 },
    paths: { cacheRoot: 'C:/a|b', logDirectory: 'D:\\mcpplsCache\\log' },
} as unknown as CacheDetail;

suite('tooltip card', () => {
    test('server strings cannot break the markdown structure', () => {
        const escaped = escapeCell('C:\\a|b [x] `y`');
        assert.ok(!/[|`[\]]/.test(escaped.replace(/\\[|`[\]\\]/g, '')), 'every metacharacter is escaped');
        assert.strictEqual(escapeCell('line1\nline2'), 'line1 line2', 'a newline cannot start a new card line');
    });

    test('the bar is a fixed-width line of the four class characters', () => {
        const bar = distributionBar(detail);
        assert.strictEqual(bar.length, 24);
        assert.ok(bar.includes('▓') && bar.includes('▒') && bar.includes('░'), 'present classes get their character');
        const empty = distributionBar({ canonical: { files: 0, bytes: 0 }, copies: { files: 0, bytes: 0 }, instances: { count: 0, bytes: 0 } });
        assert.strictEqual(empty, '', 'an all-zero cache draws an empty bar, never a division by zero');
    });

    test('the card leads with the module state, then the big number, then the four classes', () => {
        const lines = cardMarkdown('C++ Modules — Ready', {
            detail,
            coarse: undefined,
            withCommands: false,
            sweepCommand: 'mcppls.sweepWorkspaceCache',
            copyPromptCommand: 'mcppls.copyAgentPrompt',
        });
        assert.ok(lines.startsWith('**C++ Modules — Ready**'));
        assert.ok(lines.includes('缓存 3.80 GB / 4.00 GB'));
        assert.ok(lines.includes('已发布 1.90 GB'));
        assert.ok(lines.includes('副本 1.70 GB (6837 个)'));
        assert.ok(lines.includes('最老副本 90 秒前'));
        assert.ok(lines.includes('2 个未能删除'), 'failures are visible, never silent');
        assert.ok(lines.includes('点击状态栏打开清理菜单'));
    });

    test('with commands the card offers the sweep and the prompt as trusted links, and says the promise', () => {
        const lines = cacheCardLines({
            detail,
            withCommands: true,
            sweepCommand: 'mcppls.sweepWorkspaceCache',
            copyPromptCommand: 'mcppls.copyAgentPrompt',
        });
        void lines;
        const markdown = cardMarkdown('C++ Modules', {
            detail,
            withCommands: true,
            sweepCommand: 'mcppls.sweepWorkspaceCache',
            copyPromptCommand: 'mcppls.copyAgentPrompt',
        });
        assert.ok(markdown.includes('(command:mcppls.sweepWorkspaceCache)'));
        assert.ok(markdown.includes('(command:mcppls.copyAgentPrompt)'));
        assert.ok(markdown.includes('不重启引擎、不重新编译'));
    });

    test('without a detail the card falls back to the coarse status numbers', () => {
        const markdown = cardMarkdown('C++ Modules', {
            coarse: {
                bytes: 3_800_000_000,
                limitBytes: 4_000_000_000,
                state: 'ok',
                copies: { files: 3, bytes: 300 },
                instances: { count: 1, bytes: 100 },
                lastSweep: { at: Date.now(), freedBytes: 5_000_000 },
            },
            withCommands: false,
            sweepCommand: 'mcppls.sweepWorkspaceCache',
            copyPromptCommand: 'mcppls.copyAgentPrompt',
        });
        assert.ok(markdown.includes('缓存 3.80 GB / 4.00 GB'));
        assert.ok(markdown.includes('副本 300 B (3 个)'));
        assert.ok(markdown.includes('打开菜单可看明细'));
    });
});
