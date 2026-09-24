// How a `cxxModules/status` notification's issues become the words the status bar and the language
// status item show (design 2026-09-25 §6). Kept free of `vscode`, the same reason
// src/serverLog.ts is, so the categorisation and wording rules are testable in plain Node.

export type IssueCategory = 'code' | 'engine' | 'environment' | 'project';

export interface StatusIssue {
    code: string;
    message: string;
    category?: IssueCategory;
}

// A `code` issue is the user's own text being wrong (an unterminated `import`, a name that does not
// resolve): it becomes a diagnostic on that range and never colours the status bar. Everything else,
// including an issue with no `category` at all (an older server, from before this field existed),
// is treated as non-code, exactly as every issue was before this field existed.
export function isCodeIssue(issue: StatusIssue): boolean {
    return issue.category === 'code';
}

export function firstNonCodeIssue(issues: readonly StatusIssue[]): StatusIssue | undefined {
    return issues.find((issue) => !isCodeIssue(issue));
}

// A sensible short form for the status bar's one line: the first sentence if that already fits, else
// a word-boundary truncation. The full message always stays available in the tooltip and the
// language status item's detail (`full` below), so nothing here is actually lost, only not shown twice.
export function shorten(message: string, maxLength = 72): string {
    const trimmed = message.trim();
    if (trimmed.length <= maxLength) {
        return trimmed;
    }
    // A sentence-ending punctuation mark counts only when followed by whitespace or the end of the
    // string, so a dot inside a filename ("main.cpp") is not mistaken for the end of a sentence.
    const ending = /[.!?;](?=\s|$)/.exec(trimmed);
    const candidate = ending ? trimmed.slice(0, ending.index + 1) : trimmed;
    if (candidate.length <= maxLength) {
        return candidate;
    }
    const cut = candidate.slice(0, maxLength - 1);
    const lastSpace = cut.lastIndexOf(' ');
    const kept = lastSpace > maxLength / 3 ? cut.slice(0, lastSpace) : cut;
    return `${kept.trimEnd()}…`;
}

export interface StateTexts {
    // The status bar's compact suffix; undefined when a state has none (ready, with no profile to show).
    short: string | undefined;
    // The tooltip and the language status item's detail; the full text behind `short`. Equal to
    // `short` for every state except `degraded`, which is the only one that shortens anything.
    full: string | undefined;
}

function same(text: string | undefined): StateTexts {
    return { short: text, full: text };
}

export interface StatusForText {
    state: 'starting' | 'loading' | 'preparing' | 'ready' | 'degraded' | 'error';
    progress?: { done: number; total: number };
    issues?: readonly StatusIssue[];
}

// The generic "Some features are limited" wording is gone: `degraded` now names the first non-code
// issue instead. `error` keeps its fixed sentence -- module-level features really are all that is
// left, whatever the cause -- and appends why.
export function stateTexts(status: StatusForText): StateTexts {
    switch (status.state) {
        case 'starting':
            return same('Starting');
        case 'loading':
            return same('Loading the project');
        case 'preparing':
            return same(status.progress && status.progress.total > 0
                ? `Preparing modules ${status.progress.done}/${status.progress.total}`
                : 'Preparing modules');
        case 'ready':
            return same(undefined);
        case 'degraded': {
            const issue = firstNonCodeIssue(status.issues ?? []);
            const full = issue ? issue.message : 'Limited';
            return { short: issue ? shorten(issue.message) : full, full };
        }
        case 'error': {
            const base = 'Only module-level features are available';
            const issue = firstNonCodeIssue(status.issues ?? []);
            return same(issue ? `${base}: ${issue.message}` : base);
        }
    }
}
