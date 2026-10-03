// The hover card (0.0.10 plan C-13.2; v3.1 2026-10-03, live review): the read-only half of the
// cache UI, markdown all the way down -- no drawn image (tried in v3, asked back). Zones: ONE
// title line (state dot, project, state, module and unit counts, source), the optional
// preparation line while modules build, ONE table for the whole cache (a monochrome dot-matrix
// bar a class and a bold total row against the budget), the sweep line when there was one, and a
// footer of exactly two lines -- the actions and the repository -- which the review asked to
// ALIGN with each other: the narrower line is padded with no-break spaces so the two read as one
// centred block under the table. Plain spaces cannot do it (markdown folds runs of them);
// U+00A0 does not fold and does not start a code block.
//
// Layout rules, so no state surprises the shape: every bar the SAME fixed width (`█` filled,
// `░` track, in a code span); all four class rows always present, zero or not; a fact that does
// not exist (no plan, no source, no sweep, no detail) drops its own part without reshuffling the
// rest; the coarse fallback renders the same table shape. Zones are blank-line separated blocks
// (markdown folds a single newline into a space -- v1 read as one run-on paragraph), the table a
// block of its own. Pure: every server-sent string goes through `escape` first (S3 5.7).
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

// The visible width of a line the way the hover lays it out: a `$(codicon)` counts as the icon's
// two columns, a CJK or fullwidth character as its two, everything else as one. This is what the
// footer's no-break-space padding is measured with.
export function visibleWidth(text: string): number {
    let total = 0;
    let index = 0;
    while (index < text.length) {
        if (text.startsWith('$(', index)) {
            const end = text.indexOf(')', index + 2);
            if (end !== -1) {
                total += 2;
                index = end + 1;
                continue;
            }
        }
        const code = text.codePointAt(index) ?? 0;
        const wide = (code >= 0x1100 && code <= 0x115f)       // Hangul Jamo
            || (code >= 0x2e80 && code <= 0x303f)             // CJK radicals, punctuation
            || (code >= 0x3040 && code <= 0x33ff)             // kana, CJK compatibility
            || (code >= 0x3400 && code <= 0x4dbf)
            || (code >= 0x4e00 && code <= 0x9fff)             // CJK unified
            || (code >= 0xf900 && code <= 0xfaff)
            || (code >= 0xff00 && code <= 0xff60)             // fullwidth forms
            || (code >= 0xffe0 && code <= 0xffe6);
        total += wide ? 2 : 1;
        index += code > 0xffff ? 2 : 1;
    }
    return total;
}

// The state dot is a SHAPE, never a colour: `○` also says over-budget (the cache tier of the card).
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
// (two trailing spaces) inside a zone, and every zone stands alone between blank lines.
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
        // budget -- the grid keeps every column aligned. All four class rows are always there.
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

// ---------------------------------------------------------------------------
// the footer: two lines that align with each other
// ---------------------------------------------------------------------------

/** `github.com/Sunrisepeak/mcpp-language-server` -- the URL minus the protocol, the way it reads on the card. */
export function repoLabel(url: string): string {
    return url.replace(/^https?:\/\//, '').replace(/\/$/, '');
}

const NO_BREAK_SPACE = '\u00A0';

/**
 * The two footer lines as one aligned pair: the narrower line is padded at the front with
 * no-break spaces -- half the width difference -- so the actions and the repository read as one
 * centred block under the table, in either language. `text` is the raw markdown of a line;
 * `plain` is what it renders as (link labels, icons), the width the padding is measured with.
 */
export function alignedPair(first: { text: string; plain: string }, second: { text: string; plain: string }): string {
    const widths = [visibleWidth(first.plain), visibleWidth(second.plain)];
    const pad = (width: number): string => NO_BREAK_SPACE.repeat(Math.max(0, Math.floor((Math.max(...widths) - width) / 2)));
    return pad(widths[0]) + first.text + '  \n' + pad(widths[1]) + second.text;
}

/** The whole card: project first (C-13.2: the first glance is "how is the project"), the cache second, the aligned footer last. */
export function cardMarkdown(input: CardInput): string {
    const zones = [...cacheCardZones(input)];
    if (input.withCommands) {
        // Names that pair with the repository line's length (about 49 columns) in BOTH
        // languages, so the two lines read as one block without visible padding: the en
        // triple lands 3 columns wide of it, the zh 1 narrow, and the no-break-space pass
        // below closes whatever remains (at most a column or two).
        const actions = `[$(clear-all) ${t('Sweep cache')}](command:${input.sweepCommand})`
            + ` · [$(folder-opened) ${t('Open logs')}](command:${input.revealCommand}?%5B%22root%22%5D)`
            + ` · [$(copy) ${t('Copy agent prompt')}](command:${input.copyPromptCommand})`;
        const actionsPlain = `$(clear-all) ${t('Sweep cache')} · $(folder-opened) ${t('Open logs')} · $(copy) ${t('Copy agent prompt')}`;
        const repo = `[$(github) ${escapeCell(repoLabel(REPOSITORY))}](${REPOSITORY}) · [$(copy)](command:${input.copyRepositoryCommand})`;
        const repoPlain = `$(github) ${repoLabel(REPOSITORY)} · $(copy)`;
        zones.push(alignedPair({ text: actions, plain: actionsPlain }, { text: repo, plain: repoPlain }));
    } else {
        zones.push(t('Click the status bar for the menu.'));
    }
    return zones.filter((text) => text.length > 0).join('\n\n');
}
