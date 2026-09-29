// Which status issues are ones the server cannot recover from by itself, and what a person is
// offered for them (0.0.7 plan: the "unrecoverable error" experience). Pure: no `vscode`.

// The fields of a status issue this reads (S3 4; `bundle` added in 0.0.7).
export interface ModuleIssueLike {
    code: string;
    message: string;
    category?: 'code' | 'engine' | 'environment' | 'project';
    // The absolute path of a .zip the server wrote by itself for an issue it cannot recover from.
    bundle?: string;
}

// Codes for which the server writes a diagnostic bundle by itself and puts its path in `bundle`.
// preparation-stalled only carries it once it has lasted; module-lock-stale recovers and never does.
export const BUNDLE_CODES: readonly string[] = [
    'engine-crash-loop', 'engine-start-failed', 'engine-incompatible', 'payload-corrupt', 'preparation-stalled',
];

// Codes that alone justify the notification when an older server sends no bundle. preparation-stalled
// is not among them: without a bundle it may not have lasted, and the status item already offers the
// cache reset for it.
export const FATAL_CODES: readonly string[] = ['engine-crash-loop', 'engine-start-failed', 'engine-incompatible', 'payload-corrupt'];

export type FatalAction = 'reportIssue' | 'restartServer' | 'resetCache' | 'turnOff' | 'showLogs';

export const ACTION_LABELS: Record<FatalAction, string> = {
    reportIssue: 'Report Issue…',
    restartServer: 'Restart Server',
    resetCache: 'Reset This Workspace\'s Cache',
    turnOff: 'Turn Off in This Workspace',
    showLogs: 'Show Logs',
};

export interface FatalNotice {
    code: string;
    message: string;
    bundle?: string;
    // The words under the issue's own message.
    detail: string;
    actions: FatalAction[];
}

export const BUNDLE_DETAIL = 'mcppls saved a diagnostic bundle (it stays on this machine).';
export const NO_BUNDLE_DETAIL = 'mcppls could not recover from this by itself. Report Issue writes a diagnostic bundle first (it stays on this machine).';

export function bundleOf(issue: ModuleIssueLike): string | undefined {
    return typeof issue.bundle === 'string' && issue.bundle.length > 0 ? issue.bundle : undefined;
}

export function noticeFor(issue: ModuleIssueLike): FatalNotice | undefined {
    if (issue.category === 'code') return undefined;
    const bundle = bundleOf(issue);
    if (!bundle && !FATAL_CODES.includes(issue.code)) return undefined;
    return {
        code: issue.code,
        message: issue.message,
        bundle,
        detail: bundle ? BUNDLE_DETAIL : NO_BUNDLE_DETAIL,
        // Without a bundle (an older server), Report Issue exports one first.
        actions: ['reportIssue', 'restartServer', 'resetCache', 'turnOff', 'showLogs'],
    };
}

// Once per code per session; an issue that first came without a bundle and later with one is told
// again, because now there is something to attach.
export class NoticeLedger {
    private readonly seen = new Map<string, boolean>();

    // The notices to show for a status's issues, remembering them.
    take(issues: readonly ModuleIssueLike[] | undefined): FatalNotice[] {
        const notices: FatalNotice[] = [];
        for (const issue of issues ?? []) {
            const notice = noticeFor(issue);
            if (!notice) continue;
            const had = this.seen.get(notice.code);
            const has = notice.bundle !== undefined;
            if (had === undefined || (!had && has)) {
                this.seen.set(notice.code, has);
                notices.push(notice);
            }
        }
        return notices;
    }
}

export function noticeText(notice: { message: string; detail: string }): string {
    return `C++ Modules: ${notice.message} ${notice.detail}`;
}

// The crash notification (the client's own restart budget used up).
export const CRASH_DETAIL = 'mcppls saved a crash report (it stays on this machine).';
export const CRASH_ACTIONS: FatalAction[] = ['reportIssue', 'restartServer', 'turnOff', 'showLogs'];

export function crashMessage(crashes: number): string {
    return `mcppls stopped after crashing ${crashes} times`;
}
