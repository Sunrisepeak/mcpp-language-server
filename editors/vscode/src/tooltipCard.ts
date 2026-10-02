// The hover card (0.0.10 plan C-13.2, C-13.3): the read-only half of the cache UI, one markdown
// string the status bar shows on hover. Pure: it renders strings, and every string the server sent
// goes through `escape` first -- a path is text, never markdown (S3 5.7: the server is trusted to
// be true, not to be safe markup).
import { CacheDetail, CxxCacheStatus, sizeText } from './cacheSegment';

/** Turns a server-sent string into literal markdown text: pipes, backticks and brackets cannot break the card. */
export function escapeCell(text: string): string {
    return text.replace(/([\\`|[\]])/g, '\\$1').replace(/\r?\n/g, ' ');
}

const BAR_WIDTH = 24;
const BAR_CHARACTERS = { canonical: '▓', copies: '▒', instances: '░', trash: '·' } as const;

/**
 * The text bar: the four classes in one line, each with its own character (colour never carries
 * the meaning alone). All-zero stays a visible empty bar instead of dividing by zero.
 */
export function distributionBar(detail: Pick<CacheDetail, 'canonical' | 'copies' | 'trash' | 'instances'>, width = BAR_WIDTH): string {
    const parts = [
        { key: 'canonical' as const, bytes: detail.canonical?.bytes ?? 0, character: BAR_CHARACTERS.canonical },
        { key: 'copies' as const, bytes: detail.copies?.bytes ?? 0, character: BAR_CHARACTERS.copies },
        { key: 'instances' as const, bytes: detail.instances?.bytes ?? 0, character: BAR_CHARACTERS.instances },
        { key: 'trash' as const, bytes: detail.trash?.bytes ?? 0, character: BAR_CHARACTERS.trash },
    ];
    const total = parts.reduce((sum, part) => sum + part.bytes, 0);
    const cells = parts.map((part) => ({
        character: part.character,
        count: total > 0 ? Math.max(part.bytes > 0 ? 1 : 0, Math.round((part.bytes / total) * width)) : 0,
    }));
    // Rounding may overflow the width by one or two; give back from the fullest first.
    let overflow = cells.reduce((sum, cell) => sum + cell.count, 0) - width;
    for (const cell of [...cells].sort((a, b) => b.count - a.count)) {
        if (overflow <= 0) break;
        const give = Math.min(overflow, Math.max(0, cell.count - 1));
        cell.count -= give;
        overflow -= give;
    }
    return cells.map((cell) => cell.character.repeat(cell.count)).join('');
}

export interface CardInput {
    /** The coarse numbers the status already carries; the card falls back to them. */
    coarse?: CxxCacheStatus;
    /** The last detail the hub or a sweep fetched; the card prefers it. */
    detail?: CacheDetail;
    /** Command links at the tail (the trusted-command mechanism the reset link already uses). */
    withCommands?: boolean;
    sweepCommand: string;
    copyPromptCommand: string;
}

/** The cache lines of the card: the big number, the bar, the four classes, the housekeeping line. */
export function cacheCardLines(input: CardInput): string[] {
    const detail = input.detail;
    const bytes = detail?.bytes ?? input.coarse?.bytes ?? 0;
    const limit = detail?.limits.perWorkspace ?? input.coarse?.limitBytes ?? 0;
    const lines: string[] = [];
    const fill = limit > 0 ? ` / ${sizeText(limit)} (${Math.round((bytes / limit) * 100)}%)` : '';
    lines.push(`**缓存 ${sizeText(bytes)}${fill}**`);
    if (detail) {
        lines.push(`\`${escapeCell(distributionBar(detail))}\``);
        const oldest = detail.copies.oldestSeconds !== undefined && detail.copies.oldestSeconds > 0 ? ` · 最老副本 ${detail.copies.oldestSeconds} 秒前` : '';
        lines.push(
            `已发布 ${sizeText(detail.canonical?.bytes ?? 0)} · 副本 ${sizeText(detail.copies.bytes)} (${detail.copies.files} 个) · 实例 ${sizeText(detail.instances.bytes)} · 垃圾箱 ${sizeText(detail.trash?.bytes ?? 0)}${oldest}`,
        );
        if (detail.lastSweep && detail.lastSweep.at > 0) {
            const age = Math.max(1, Math.round((Date.now() - detail.lastSweep.at) / 1000));
            const failed = detail.lastSweep.failed ? `，${detail.lastSweep.failed} 个未能删除` : '';
            lines.push(`上次清理 ${age < 60 ? `${age} 秒前` : `${Math.round(age / 60)} 分钟前`}${failed}（释放 ${sizeText(detail.lastSweep.freedBytes)} / ${detail.lastSweep.files} 个）`);
        }
        lines.push(`日志目录：${escapeCell(detail.paths.logDirectory)}`);
    } else if (input.coarse) {
        lines.push(`副本 ${sizeText(input.coarse.copies.bytes)} (${input.coarse.copies.files} 个) · 实例 ${sizeText(input.coarse.instances.bytes)} (${input.coarse.instances.count} 个)`);
        if (input.coarse.lastSweep) {
            lines.push(`上次清理释放 ${sizeText(input.coarse.lastSweep.freedBytes)}`);
        }
        lines.push('打开菜单可看明细。');
    }
    return lines;
}

/** The whole card, module state first (C-13.2: the first glance is "how is the project", the cache is the second). */
export function cardMarkdown(moduleLine: string, input: CardInput): string {
    const lines = [`**${escapeCell(moduleLine)}**`, '', ...cacheCardLines(input)];
    if (input.withCommands) {
        lines.push('', `[$(clear-all) 清理缓存](command:${input.sweepCommand}) · [$(copy) 复制 Agent 提示词](command:${input.copyPromptCommand})`, '');
        lines.push('_清理不重启引擎、不重新编译；日志不会离开本机。_');
    } else {
        lines.push('', '_点击状态栏打开清理菜单。_');
    }
    return lines.join('\n');
}
