// When to offer to fetch what the build description needs (plan 2026-09-27 B-2, §9.2): the decision of
// downloadPrompt.ts, in plain TypeScript so it is tested without VS Code (test/unit/downloadAsk.test.ts).

export const NEEDS_DOWNLOAD_CODE = 'producer-needs-download';

export interface DownloadIssue {
    code: string;
    message: string;
    // S3 (plan 2026-09-27 B-2): the server lets a client offer to fetch what is missing.
    askOnline?: boolean;
}

export function needsDownload(status: { issues?: { code: string; message: string }[] }): DownloadIssue | undefined {
    return (status.issues ?? []).find((issue) => issue.code === NEEDS_DOWNLOAD_CODE) as DownloadIssue | undefined;
}

// Whether to ask now: the server offers it, the person has not declined for this workspace, this set of
// missing things has not been asked about yet, and no question for this root is still open.
export function shouldAsk(issue: DownloadIssue | undefined, never: boolean, askedAbout: readonly string[], open: boolean): boolean {
    return issue !== undefined && issue.askOnline === true && !never && !open && !askedAbout.includes(issue.message);
}

// D-4 (plan 0.0.9): what to do about a download the server offers. With downloads allowed for this workspace
// ("Always Download in This Workspace") it is fetched without asking -- once per set of missing things, like the
// question -- and otherwise asked, unless the person said "Don't Ask Again". A run already open for this root waits.
export type DownloadAction = 'ask' | 'fetch' | 'none';

export function downloadAction(issue: DownloadIssue | undefined, never: boolean, allowed: boolean, askedAbout: readonly string[], open: boolean): DownloadAction {
    if (issue === undefined || issue.askOnline !== true || open || askedAbout.includes(issue.message)) return 'none';
    if (allowed) return 'fetch';
    return never ? 'none' : 'ask';
}

// D-5 (plan 0.0.9): how the last fetch the person asked for ended, as the server says it in the status (S3
// `onlineRun`). `at` tells one run from the next, so each is told once.
export interface OnlineRun {
    outcome: 'fetched' | 'failed';
    message: string;
    at: string;
}

export function onlineRunToTell(status: { onlineRun?: OnlineRun }, told: readonly string[]): OnlineRun | undefined {
    const run = status.onlineRun;
    if (run === undefined || typeof run.at !== 'string' || told.includes(run.at)) return undefined;
    return run.outcome === 'fetched' || run.outcome === 'failed' ? run : undefined;
}
