// The hover card (0.0.10 plan C-13.2; v3 2026-10-03, live review): the read-only half of the
// cache UI. Markdown hovers give NO layout control -- VS Code strips every style attribute, so
// text cannot be centred, spaced or aligned beyond what paragraphs happen to do, and five rounds
// of markdown reshaping could not buy the composition the review kept asking for. What a hover
// DOES carry faithfully is images: so the card BODY is one self-drawn SVG (data URI, nothing
// fetched), with real layout -- centred title, column x positions, right-aligned numbers,
// dot-matrix bars on one grid, even spacing, theme-aware text colours. The interactive part stays
// real markdown: the action links and the repository line under the image, because a link inside
// an image is not a link. Pure: every server-sent string is escaped into the SVG as text, never
// markup (S3 5.7: the server is trusted to be true, not safe), and the words still go through
// `strings.ts`, so the image is bilingual like everything else.
import { CacheDetail, CxxCacheStatus, sizeText } from './cacheSegment';
import { REPOSITORY } from './issueUrl';
import { t } from './strings';

// ---------------------------------------------------------------------------
// small pure helpers, all unit-tested
// ---------------------------------------------------------------------------

/** Turns a server-sent string into literal markdown text: pipes, backticks and brackets cannot break the card. */
export function escapeCell(text: string): string {
    return text.replace(/([\\`|[\]])/g, '\\$1').replace(/\r?\n/g, ' ');
}

/** Turns a server-sent string into literal SVG text content: no tag of its own can survive. */
export function escapeSvg(text: string): string {
    return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&apos;');
}

/** The last segment of a path, whichever separator it came with. */
export function baseName(path: string): string {
    const cut = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return cut === -1 ? path : path.slice(cut + 1);
}

// The state dot is a SHAPE (never a colour alone): filled, half, open. Over-budget reads `open`
// too -- the cache tier of the card.
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
    /** The editor's colour theme, so the drawn card reads on both: 'dark' | 'light'. */
    theme?: 'dark' | 'light';
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

// ---------------------------------------------------------------------------
// the drawn card
// ---------------------------------------------------------------------------

// The canvas and its column positions -- the layout the review asked for, as numbers: a centred
// header, one grid for the rows, right-aligned numbers, bars ending on one edge, air everywhere.
const WIDTH = 400;
const MARGIN = 16;
const LABEL_X = MARGIN;                      // class names, anchored start
const USED_END = 158;                        // sizes, anchored end
const SHARE_END = 210;                       // percents, anchored end
const BAR_X = 224;                           // the dot-matrix grid starts here
const CELLS = 12;
const CELL_W = 12;
const CELL_GAP = 2;
const CELL_H = 8;
const ROW_H = 18;
// No quotes in the stack: the SVG's attributes are single-quoted, and a quoted font name inside
// would break the markup. Multiword names read fine unquoted here.
const FONT = '-apple-system, Segoe UI, Ubuntu, Noto Sans, sans-serif';

const THEME: Record<'dark' | 'light', { text: string; strong: string; dim: string; line: string; fill: number; track: number; ink: string }> = {
    dark: { text: '#cccccc', strong: '#e8e8e8', dim: '#8a8a8a', line: '#3c3c3c', fill: 0.9, track: 0.18, ink: '#cccccc' },
    light: { text: '#3f3f3f', strong: '#1f1f1f', dim: '#767676', line: '#d0d0d0', fill: 0.85, track: 0.14, ink: '#3f3f3f' },
};

interface Row {
    label: string;
    used: string;
    percent: number;
    share: number;
}

function barRects(x: number, y: number, share: number, colors: typeof THEME.dark): string {
    const clamped = Number.isFinite(share) ? Math.min(1, Math.max(0, share)) : 0;
    const filled = Math.round(clamped * CELLS);
    let out = '';
    for (let index = 0; index < CELLS; index += 1) {
        out += `<rect x='${x + index * (CELL_W + CELL_GAP)}' y='${y}' width='${CELL_W}' height='${CELL_H}' rx='2' `
            + `fill='${colors.ink}' fill-opacity='${index < filled ? colors.fill : colors.track}'/>`;
    }
    return out;
}

/** The card body as one SVG document: every x is a decision, every gap is a number. */
export function svgCard(input: CardInput): string {
    const colors = input.theme === 'light' ? THEME.light : THEME.dark;
    const detail = input.detail;
    const coarse = input.coarse;
    const text = (x: number, y: number, content: string, anchor: 'start' | 'middle' | 'end', size: number, weight: 400 | 600, color: string): string =>
        `<text x='${x}' y='${y}' text-anchor='${anchor}' font-family='${FONT}' font-size='${size}' font-weight='${weight}' fill='${color}'>${escapeSvg(content)}</text>`;

    // -- gather the facts, each dropping out cleanly when it does not exist (layout stays put)
    const bytes = detail?.bytes ?? coarse?.bytes ?? 0;
    const limit = detail?.limits.perWorkspace ?? coarse?.limitBytes ?? 0;
    const fill = limit > 0 ? Math.min(1, bytes / limit) : 0;
    const percent = limit > 0 ? Math.round(fill * 100) : 0;
    const total = Math.max(1, bytes);

    const header: string[] = [];
    if (input.status) {
        const facts: string[] = [];
        if (detail?.plan && (detail.plan.modules > 0 || detail.plan.units > 0)) {
            facts.push(t('{0} modules · {1} units', detail.plan.modules, detail.plan.units));
        }
        if (input.status.source) facts.push(input.status.source);
        const tail = facts.length > 0 ? ` · ${facts.join(' · ')}` : '';
        const budget = Math.max(12, 34 - tail.length);
        const name = baseName(input.status.root);
        const shown = name.length > budget ? `${name.slice(0, budget - 1)}…` : name;
        header.push(`${stateDot(input.status.state, coarse?.state)} ${shown} — ${stateWord(input.status.state)}${tail}`);
    }

    const rows: Row[] = detail
        ? [
            { label: t('Published'), used: sizeText(detail.canonical?.bytes ?? 0), percent: Math.round(((detail.canonical?.bytes ?? 0) / total) * 100), share: (detail.canonical?.bytes ?? 0) / total },
            { label: t('Copies'), used: sizeText(detail.copies.bytes), percent: Math.round((detail.copies.bytes / total) * 100), share: detail.copies.bytes / total },
            { label: t('Instances'), used: sizeText(detail.instances.bytes), percent: Math.round((detail.instances.bytes / total) * 100), share: detail.instances.bytes / total },
            { label: t('Trash'), used: sizeText(detail.trash?.bytes ?? 0), percent: Math.round(((detail.trash?.bytes ?? 0) / total) * 100), share: (detail.trash?.bytes ?? 0) / total },
        ]
        : [];

    // -- place everything: header centred, the grid from a fixed top, footer under a rule
    let y = 22;
    const parts: string[] = [];
    if (header.length > 0) {
        parts.push(text(WIDTH / 2, y, header[0], 'middle', 13, 600, colors.strong));
        y += 12;   // room for the optional preparation line
        const progress = input.status?.progress ?? detail?.progress;
        if (progress && progress.total > 0) {
            const share = progress.done / progress.total;
            y += 8;
            parts.push(text(MARGIN, y, t('Preparing index {0}/{1}', progress.done, progress.total), 'start', 10.5, 400, colors.dim));
            parts.push(barRects(BAR_X, y - 8, share, colors));
            parts.push(text(WIDTH - MARGIN, y, `${Math.round(share * 100)}%`, 'end', 10.5, 400, colors.dim));
            y += 14;
        }
        y += 6;
    }

    const gridTop = y + 6;
    y = gridTop;
    for (const row of rows) {
        parts.push(text(LABEL_X, y, row.label, 'start', 11, 400, colors.text));
        parts.push(text(USED_END, y, row.used, 'end', 11, 400, colors.text));
        parts.push(text(SHARE_END, y, `${row.percent}%`, 'end', 11, 400, colors.dim));
        parts.push(barRects(BAR_X, y - 8, row.share, colors));
        y += ROW_H;
    }

    if (rows.length > 0) {
        y += 2;
        parts.push(`<line x1='${MARGIN}' y1='${y}' x2='${WIDTH - MARGIN}' y2='${y}' stroke='${colors.line}' stroke-width='1'/>`);
        y += 14;
    }

    // The total against the budget -- bold, the widest row, the numbers the card exists for.
    const totalText = limit > 0 ? `${sizeText(bytes)} / ${sizeText(limit)}` : sizeText(bytes);
    parts.push(text(LABEL_X, y, t('Total / budget'), 'start', 11.5, 600, colors.strong));
    parts.push(text(SHARE_END, y, limit > 0 ? `${percent}%` : '', 'end', 11.5, 600, colors.strong));
    parts.push(barRects(BAR_X, y - 8, fill, colors));
    parts.push(text(WIDTH - MARGIN, y, totalText, 'end', 11.5, 600, colors.strong));
    y += 18;

    if (detail?.lastSweep && detail.lastSweep.at > 0) {
        const age = Math.max(1, Math.round((Date.now() - detail.lastSweep.at) / 1000));
        const failed = detail.lastSweep.failed ? t(', {0} failed to delete', detail.lastSweep.failed) : '';
        parts.push(text(WIDTH / 2, y, t('Last sweep {0}: freed {1} ({2} files){3}', ageText(age), sizeText(detail.lastSweep.freedBytes), detail.lastSweep.files, failed),
            'middle', 10, 400, colors.dim));
        y += 14;
    } else if (!detail && coarse) {
        parts.push(text(WIDTH / 2, y, t('Copies {0} ({1} files) · instances {2} ({3})', sizeText(coarse.copies.bytes), coarse.copies.files,
            sizeText(coarse.instances.bytes), coarse.instances.count), 'middle', 10, 400, colors.dim));
        y += 14;
    }

    const height = y + 4;
    return `<svg xmlns='http://www.w3.org/2000/svg' width='${WIDTH}' height='${height}'>${parts.join('')}</svg>`;
}

// ---------------------------------------------------------------------------
// the whole hover: the drawn body, then the real links
// ---------------------------------------------------------------------------

/** `github.com/Sunrisepeak/mcpp-language-server` -- the URL minus the protocol, the way it reads on the card. */
export function repoLabel(url: string): string {
    return url.replace(/^https?:\/\//, '').replace(/\/$/, '');
}

/** The card's one-line alt summary: what shows if an image ever fails -- the facts, text-only. */
export function cardAlt(input: CardInput): string {
    const bytes = input.detail?.bytes ?? input.coarse?.bytes ?? 0;
    const limit = input.detail?.limits.perWorkspace ?? input.coarse?.limitBytes ?? 0;
    const name = input.status ? baseName(input.status.root) : 'cache';
    return limit > 0 ? `${name}: ${sizeText(bytes)} / ${sizeText(limit)}` : `${name}: ${sizeText(bytes)}`;
}

/** The whole card: the drawn body as an image, the actions and the repository as real links. */
export function cardMarkdown(input: CardInput): string {
    const lines: string[] = [];
    lines.push(`![${escapeCell(cardAlt(input))}](data:image/svg+xml;utf8,${encodeURIComponent(svgCard(input))})`);
    if (input.withCommands) {
        // One short line (the hub and the palette carry the full names); the image above set the
        // width, so the links sit under the card, not beside it.
        lines.push(`[$(clear-all) ${t('Sweep')}](command:${input.sweepCommand})`
            + ` · [$(folder-opened) ${t('Logs')}](command:${input.revealCommand}?%5B%22root%22%5D)`
            + ` · [$(copy) ${t('Agent prompt')}](command:${input.copyPromptCommand})`);
        lines.push(`[$(github) ${escapeCell(repoLabel(REPOSITORY))}](${REPOSITORY}) · [$(copy)](command:${input.copyRepositoryCommand})`);
    } else {
        lines.push(t('Click the status bar for the menu.'));
    }
    return lines.join('\n\n');
}
