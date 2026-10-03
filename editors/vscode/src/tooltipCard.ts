// The hover card (0.0.10 plan C-13.2; v2.4 2026-10-03, live-review): the read-only half of the
// cache UI, one markdown string the status bar shows on hover. Zones: ONE title line (state dot,
// project, state, module and unit counts, source -- everything the project is), the optional
// preparation line while modules build, ONE table for the whole cache (a monochrome dot-matrix
// bar a class and a bold total row against the budget), the sweep line when there was one, the
// actions, and the repository link. Pure: every server-sent string goes through `escape` first --
// a path is text, never markdown (S3 5.7: the server is trusted to be true, not safe markup).
//
// Layout rules, so no state surprises the shape (the review's "对齐感/不乱"):
// - every bar is the SAME fixed width (`█` filled, `░` track, in a code span), so the chart column
//   aligns by construction; colour is not used at all -- hover text cannot carry it honestly, and
//   the dot shapes plus the percent column carry the tiers;
// - the title line holds project + state + counts + source, clamped so it stays one line;
// - a fact that does not exist (no plan, no source, no sweep yet, no detail) simply drops its own
//   part, never reshuffles the rest: the table's rows are always all four classes;
// - zones are blank-line separated blocks, rows inside a zone hard-broken (markdown folds a single
//   newline into a space -- v1 read as one run-on paragraph), the table a block of its own.
import { CacheDetail, CxxCacheStatus, sizeText } from './cacheSegment';
import { REPOSITORY } from './issueUrl';
import { t } from './strings';

/** Turns a server-sent string into literal markdown text: pipes, backticks and brackets cannot break the card. */
export function escapeCell(text: string): string {
    return text.replace(/([\\`|[\]])/g, '\\$1').replace(/\r?\n/g, ' ');
}

const BAR_CELLS = 12;

/** The dot-matrix bar, monochrome: `█` for the filled share, `░` for the scale behind it. */
export function bar(share: number, cells = BAR_CELLS): string {
    const clamped = Number.isFinite(share) ? Math.min(1, Math.max(0, share)) : 0;
    const filled = Math.round(clamped * cells);
    return `\`${'█'.repeat(filled)}${'░'.repeat(cells - filled)}\``;
}

/** The last segment of a path, whichever separator it came with. */
export function baseName(path: string): string {
    const cut = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return cut === -1 ? path : path.slice(cut + 1);
}

// UI-3: the state dot is a SHAPE, never a colour -- `○` also says over-budget: the cache tier.
export function stateDot(state: CardStatus['state'], cacheState: CxxCacheStatus['state'] | undefined): string {
    if (state === 'error' || cacheState === 'over') return '○';
    if (state === 'degraded') return '◐';
    return '●';
}

function stateWord(state: CardStatus['state']): string {
    switch (state) {
        case 'starting': return t('Starting');
        case 'loading': return t('Loading');
        case 'preparing': return t('Preparing');
        case 'ready': return t('Ready');
        case 'degraded': return t('Degraded');
        case 'error': return t('Error');
    }
}

/** The project zone's facts, as `status.ts` maps its status notification down to (no engine names, D17). */
export interface CardStatus {
    state: 'starting' | 'loading' | 'preparing' | 'ready' | 'degraded' | 'error';
    root: string;
    source?: string;
    progress?: { done: number; total: number };
}

export interface CardInput {
    /** The project zone; without it the card starts at the cache (an old server's coarse numbers). */
    status?: CardStatus;
    /** The coarse numbers the status already carries; the card falls back to them. */
    coarse?: CxxCacheStatus;
    /** The last detail the hub or a sweep fetched; the card prefers it. */
    detail?: CacheDetail;
    /** The action links and the repository line; without them the card says where the menu is. */
    withCommands?: boolean;
    sweepCommand: string;
    revealCommand: string;
    copyPromptCommand: string;
    copyRepositoryCommand: string;
}

function ageText(seconds: number): string {
    return seconds < 60 ? t('{0} s ago', seconds) : t('{0} min ago', Math.round(seconds / 60));
}

/** One row of the composition table: the class, its size right-aligned, its share, its bar. */
function compositionRow(label: string, bytes: number, total: number): string {
    const share = total > 0 ? bytes / total : 0;
    const percent = total > 0 ? Math.round(share * 100) : 0;
    return `| ${label} | ${sizeText(bytes)} | ${percent}% | ${bar(share)} |`;
}

// Markdown folds a single newline into a space; a row only gets its own line from a HARD break
// (two trailing spaces) inside a zone, and every zone stands alone between blank lines -- and the
// table needs its own block or the rows render as text.
function zone(rows: string[]): string {
    return rows.filter((row) => row.length > 0).join('  \n');
}

/** The card's zones: the title line, the optional preparation line, the one cache table, history. */
export function cacheCardZones(input: CardInput): string[] {
    const detail = input.detail;
    const coarse = input.coarse;
    const zones: string[] = [];

    if (input.status) {
        // One line for what the project IS: dot, name, state, counts, source -- each part drops
        // out cleanly when it does not exist, and the name clamps only enough to keep the whole
        // line under a length a hover shows without wrapping (about 70 columns).
        const facts: string[] = [];
        if (detail?.plan && (detail.plan.modules > 0 || detail.plan.units > 0)) {
            facts.push(t('{0} modules · {1} units', detail.plan.modules, detail.plan.units));
        }
        if (input.status.source) facts.push(escapeCell(input.status.source));
        const tail = facts.length > 0 ? ` · ${facts.join(' · ')}` : '';
        const budget = Math.max(12, 60 - tail.length);
        const name = baseName(input.status.root);
        const shown = name.length > budget ? `${name.slice(0, budget - 1)}…` : name;
        zones.push(`${stateDot(input.status.state, coarse?.state)} **${shown} — ${stateWord(input.status.state)}**${tail}`);
        const progress = input.status.progress ?? detail?.progress;
        if (progress && progress.total > 0) {
            const share = progress.done / progress.total;
            zones.push(`${t('Preparing index {0}/{1}', progress.done, progress.total)} ${bar(share)} ${Math.round(share * 100)}%`);
        }
    }

    const bytes = detail?.bytes ?? coarse?.bytes ?? 0;
    const limit = detail?.limits.perWorkspace ?? coarse?.limitBytes ?? 0;
    const fill = limit > 0 ? Math.min(1, bytes / limit) : 0;
    const percent = limit > 0 ? Math.round(fill * 100) : 0;
    if (detail) {
        // One table for the whole cache zone: the classes, then the bold total row against the
        // budget -- the grid keeps every column aligned, and there is no separate headline block
        // to drift out of line with it. All four class rows are always there, zero or not.
        const total = Math.max(1, bytes);
        const table = [
            `| ${t('Class')} | ${t('Used')} | ${t('Share')} |  |`,
            '|---|---:|---:|:--|',
            compositionRow(t('Published'), detail.canonical?.bytes ?? 0, total),
            compositionRow(t('Copies'), detail.copies.bytes, total),
            compositionRow(t('Instances'), detail.instances.bytes, total),
            compositionRow(t('Trash'), detail.trash?.bytes ?? 0, total),
        ];
        if (limit > 0) {
            table.push(`| **${t('Total / budget')}** | **${sizeText(bytes)} / ${sizeText(limit)}** | **${percent}%** | ${bar(fill)} |`);
        } else {
            table.push(`| **${t('Total / budget')}** | **${sizeText(bytes)}** |  | ${bar(fill)} |`);
        }
        zones.push(zone(table));
        if (detail.lastSweep && detail.lastSweep.at > 0) {
            const age = Math.max(1, Math.round((Date.now() - detail.lastSweep.at) / 1000));
            const failed = detail.lastSweep.failed ? t(', {0} failed to delete', detail.lastSweep.failed) : '';
            zones.push(t('Last sweep {0}: freed {1} ({2} files){3}', ageText(age), sizeText(detail.lastSweep.freedBytes), detail.lastSweep.files, failed));
        }
    } else if (coarse) {
        // No detail yet (or an old server): the total row against the budget in the same table
        // shape the full card will show, so the card never changes form while the detail loads.
        zones.push(zone([
            `| ${t('Used / budget')} | ${t('Share')} |  |`,
            '|---:|---:|:--|',
            `| **${sizeText(coarse.bytes)} / ${sizeText(coarse.limitBytes)}** | ${percent}% | ${bar(fill)} |`,
        ]));
        zones.push(t('Copies {0} ({1} files) · instances {2} ({3})', sizeText(coarse.copies.bytes), coarse.copies.files,
                     sizeText(coarse.instances.bytes), coarse.instances.count));
        if (coarse.lastSweep) {
            zones.push(t('The last sweep freed {0}', sizeText(coarse.lastSweep.freedBytes)));
        }
    }
    return zones;
}

/** `github.com/Sunrisepeak/mcpp-language-server` -- the URL minus the protocol, the way it reads on the card. */
export function repoLabel(url: string): string {
    return url.replace(/^https?:\/\//, '').replace(/\/$/, '');
}

/** The whole card: project first (C-13.2: the first glance is "how is the project", the cache is the second). */
export function cardMarkdown(input: CardInput): string {
    const zones = [...cacheCardZones(input)];
    if (input.withCommands) {
        // One action a line, stacked under the table like a menu: the table is then the WIDEST
        // block in every language, the panel keeps one width, and no single-line row of joined
        // links stretches the right side past the grid (the review's ragged-right complaint).
        zones.push(zone([
            `[$(clear-all) ${t('Sweep cache')}](command:${input.sweepCommand})`,
            `[$(folder-opened) ${t('Open logs & reports')}](command:${input.revealCommand}?%5B%22root%22%5D)`,
            `[$(copy) ${t('Copy agent prompt')}](command:${input.copyPromptCommand})`,
        ]));
        zones.push(`[$(github) ${escapeCell(repoLabel(REPOSITORY))}](${REPOSITORY}) · [$(copy)](command:${input.copyRepositoryCommand})`);
    } else {
        zones.push(t('Click the status bar for the menu.'));
    }
    return zones.filter((text) => text.length > 0).join('\n\n');
}
