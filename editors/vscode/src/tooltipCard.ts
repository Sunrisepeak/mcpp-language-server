// The hover card (0.0.10 plan C-13.2; v2 2026-10-03 UI-2..UI-7; v2.2 same day, review): the
// read-only half of the cache UI, one markdown string the status bar shows on hover. Zones: the
// project (state dot, module and unit counts, the real preparation progress), the cache (ONE
// table: a colored bar a class and a bold total row against the budget), the history line, and
// the actions plus where the project lives (the repository link, in place of the old footnote).
// Pure: it renders strings, and every string the server sent goes through `escape` first -- a path
// is text, never markdown (S3 5.7: the server is trusted to be true, not to be safe markup).
//
// What a hover can and cannot do (UI-7, written down so nobody looks for it again): VS Code strips
// style attributes from hover markdown, so TEXT cannot be coloured -- no span, no font, no class.
// What markdown does carry is images, so the bars are tiny self-drawn SVG dot-matrix strips (a
// data URI, nothing fetched): the cell design the first review liked, each cell actually coloured,
// the empty cells a 20% tint of the same hue as the track. Each row's own name is the legend and
// the percent column carries the number, so colour never has to be read on its own. The image's
// ALT text is the old `█░` run: if images ever fail to render, the bar degrades to the character
// design instead of disappearing. Hover text is not selectable either, so "copy" is a command link.
import { CacheDetail, CxxCacheStatus, sizeText } from './cacheSegment';
import { REPOSITORY } from './issueUrl';
import { t } from './strings';

/** Turns a server-sent string into literal markdown text: pipes, backticks and brackets cannot break the card. */
export function escapeCell(text: string): string {
    return text.replace(/([\\`|[\]])/g, '\\$1').replace(/\r?\n/g, ' ');
}

// The dot-matrix strip: `CELLS` cells, each a rounded rect; filled ones solid, empty ones a tint
// of the same colour. 12 cells at 7x9 with 2 between reads at a glance without shouting.
const CELLS = 12;
const CELL_WIDTH = 7;
const CELL_HEIGHT = 9;
const CELL_GAP = 2;

// One colour a class, everywhere the class appears: blue is what is published and usable, orange
// the copy-on-read leftover, purple the per-instance directories, brown the trash; green is the
// budget while it is fine, yellow near it, red over it. Muted, VS-Code-adjacent hues.
const CLASS_COLOR = { published: '#59a4ff', copies: '#e2a03f', instances: '#b180d7', trash: '#a07850' } as const;
const BUDGET_COLOR = { ok: '#3fb950', near: '#d29922', over: '#f85149' } as const;

export function budgetColor(level: CxxCacheStatus['state'] | 'preparing'): string {
    if (level === 'over') return BUDGET_COLOR.over;
    if (level === 'near') return BUDGET_COLOR.near;
    return BUDGET_COLOR.ok;
}

/** The dot-matrix strip as an inline image: `filled` cells solid, the rest a 20% tint, alt `█░`. */
export function cellBar(color: string, share: number, cells = CELLS): string {
    const clamped = Number.isFinite(share) ? Math.min(1, Math.max(0, share)) : 0;
    const filled = Math.round(clamped * cells);
    const rects: string[] = [];
    for (let index = 0; index < cells; index += 1) {
        rects.push(`<rect x='${index * (CELL_WIDTH + CELL_GAP)}' y='0' width='${CELL_WIDTH}' height='${CELL_HEIGHT}' rx='1.5' `
            + `fill='${color}' fill-opacity='${index < filled ? '0.95' : '0.2'}'/>`);
    }
    const width = cells * (CELL_WIDTH + CELL_GAP) - CELL_GAP;
    const svg = `<svg xmlns='http://www.w3.org/2000/svg' width='${width}' height='${CELL_HEIGHT}'>${rects.join('')}</svg>`;
    const alt = '█'.repeat(filled) + '░'.repeat(cells - filled);
    return `![${alt}](data:image/svg+xml;utf8,${encodeURIComponent(svg)})`;
}

/** The last segment of a path, whichever separator it came with. */
export function baseName(path: string): string {
    const cut = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return cut === -1 ? path : path.slice(cut + 1);
}

// UI-3: the state dot is a SHAPE, never a colour alone -- hover markdown cannot carry colour
// anyway, and a shape reads in every theme. `○` also says over-budget: the cache tier of the card.
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

/** One row of the composition table: the class, its size right-aligned, its share, its own color's bar. */
function compositionRow(label: string, color: string, bytes: number, total: number): string {
    const share = total > 0 ? bytes / total : 0;
    const percent = total > 0 ? Math.round(share * 100) : 0;
    return `| ${label} | ${sizeText(bytes)} | ${percent}% | ${cellBar(color, share)} |`;
}

// Markdown folds a single newline into a space; a row only gets its own line from a HARD break
// (two trailing spaces) inside a zone, and every zone stands alone between blank lines -- and the
// table needs its own block or the rows render as text. This is why v1 read as one long paragraph.
function zone(rows: string[]): string {
    return rows.filter((row) => row.length > 0).join('  \n');
}

/** The card's zones: project, the one cache table, history. Each is one markdown block. */
export function cacheCardZones(input: CardInput): string[] {
    const detail = input.detail;
    const coarse = input.coarse;
    const zones: string[] = [];

    if (input.status) {
        const project: string[] = [];
        const name = baseName(input.status.root);
        const shown = name.length > 28 ? `${name.slice(0, 27)}…` : name;
        project.push(`${stateDot(input.status.state, coarse?.state)} **${escapeCell(shown)} — ${stateWord(input.status.state)}**`);
        if (detail?.plan && (detail.plan.modules > 0 || detail.plan.units > 0)) {
            const source = input.status.source ? ` · ${escapeCell(input.status.source)}` : '';
            project.push(t('{0} modules · {1} units', detail.plan.modules, detail.plan.units) + source);
        }
        const progress = input.status.progress ?? detail?.progress;
        if (progress && progress.total > 0) {
            const share = progress.done / progress.total;
            project.push(`${t('Preparing index {0}/{1}', progress.done, progress.total)} ${cellBar(BUDGET_COLOR.ok, share)} ${Math.round(share * 100)}%`);
        }
        zones.push(zone(project));
    }

    const bytes = detail?.bytes ?? coarse?.bytes ?? 0;
    const limit = detail?.limits.perWorkspace ?? coarse?.limitBytes ?? 0;
    const fill = limit > 0 ? Math.min(1, bytes / limit) : 0;
    const percent = limit > 0 ? Math.round(fill * 100) : 0;
    const level: CxxCacheStatus['state'] = coarse?.state ?? (detail?.limits.over ? 'over' : 'ok');
    if (detail) {
        // One table for the whole cache zone: the classes, then the bold total row against the
        // budget -- the grid keeps every column aligned, and there is no separate headline block
        // to drift out of line with it.
        const total = Math.max(1, bytes);
        const table = [
            `| ${t('Class')} | ${t('Used')} | ${t('Share')} |  |`,
            '|---|---:|---:|:--|',
            compositionRow(t('Published'), CLASS_COLOR.published, detail.canonical?.bytes ?? 0, total),
            compositionRow(t('Copies'), CLASS_COLOR.copies, detail.copies.bytes, total),
            compositionRow(t('Instances'), CLASS_COLOR.instances, detail.instances.bytes, total),
            compositionRow(t('Trash'), CLASS_COLOR.trash, detail.trash?.bytes ?? 0, total),
        ];
        if (limit > 0) {
            table.push(`| **${t('Total / budget')}** | **${sizeText(bytes)} / ${sizeText(limit)}** | **${percent}%** | ${cellBar(budgetColor(level), fill)} |`);
        } else {
            table.push(`| **${t('Total / budget')}** | **${sizeText(bytes)}** |  | ${cellBar(budgetColor(level), fill)} |`);
        }
        zones.push(zone(table));
        if (detail.lastSweep && detail.lastSweep.at > 0) {
            const age = Math.max(1, Math.round((Date.now() - detail.lastSweep.at) / 1000));
            const failed = detail.lastSweep.failed ? t(', {0} failed to delete', detail.lastSweep.failed) : '';
            zones.push(t('Last sweep {0}: freed {1} ({2} files){3}', ageText(age), sizeText(detail.lastSweep.freedBytes), detail.lastSweep.files, failed));
        }
    } else if (coarse) {
        // No detail yet (or an old server): the total row against the budget is the one chart the
        // coarse numbers own, in the same table shape the full card will show.
        zones.push(zone([
            `| ${t('Used / budget')} | ${t('Share')} |  |`,
            '|---:|---:|:--|',
            `| **${sizeText(coarse.bytes)} / ${sizeText(coarse.limitBytes)}** | ${percent}% | ${cellBar(budgetColor(level), fill)} |`,
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
        zones.push(`[$(clear-all) ${t('Sweep cache')}]`
            + `(command:${input.sweepCommand}) · [$(folder-opened) ${t('Open logs & reports')}](command:${input.revealCommand}?%5B%22root%22%5D)`
            + ` · [$(copy) ${t('Copy agent prompt')}](command:${input.copyPromptCommand})`);
        zones.push(`[$(github) ${escapeCell(repoLabel(REPOSITORY))}](${REPOSITORY}) · [$(copy)](command:${input.copyRepositoryCommand})`);
    } else {
        zones.push(t('Click the status bar for the menu.'));
    }
    return zones.filter((text) => text.length > 0).join('\n\n');
}
