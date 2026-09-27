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
