// When to offer to install the semantic kit of a target (ecosystem design 2026-10-29 §9.2, S3-4-37): the
// decision of kitPrompt.ts, in plain TypeScript so it is tested without VS Code (test/unit/kitAsk.test.ts).
//
// The server reads a project without a compiler with the semantic kit of its target. When that kit is not
// installed it reads with the openkal kit the payload carries -- `import std` and the standard library work,
// the system's own headers and macros do not -- and says so in a `kit-missing` issue, with `askInstall` when
// it can install the kit in the background (`mcppls.installKit`).

export const KIT_MISSING_CODE = 'kit-missing';

export interface KitIssue {
    code: string;
    message: string;
    // S3-4-37: what would serve the target, and the command that installs it by hand.
    kit?: { target: string; package: string; version?: string; command: string };
    // S3-4-38: the server installs it in the background when asked (`mcppls.installKit`).
    askInstall?: boolean;
}

export function kitMissing(status: { issues?: { code: string; message: string }[] }): KitIssue | undefined {
    return (status.issues ?? []).find((issue) => issue.code === KIT_MISSING_CODE) as KitIssue | undefined;
}

// What to do about a kit the server offers to install: install it without asking when the person said kits may
// always be installed (once per kit, like the question), ask otherwise unless they said "Don't Ask Again", and
// nothing while a question about this root is open or this kit was asked about already.
export type KitAction = 'ask' | 'install' | 'none';

export function kitAction(issue: KitIssue | undefined, never: boolean, always: boolean, askedAbout: readonly string[], open: boolean): KitAction {
    if (issue === undefined || issue.askInstall !== true || open) return 'none';
    const key = kitKey(issue);
    if (askedAbout.includes(key)) return 'none';
    if (always) return 'install';
    return never ? 'none' : 'ask';
}

// One question per kit: the package and its version.
export function kitKey(issue: KitIssue): string {
    return issue.kit ? `${issue.kit.package}@${issue.kit.version ?? ''}` : issue.message;
}

// S3-4-39: how the last install the person asked for ended. `at` tells one install from the next, so each is told once.
export interface KitInstall {
    outcome: 'installed' | 'failed';
    target: string;
    message: string;
    at: string;
}

export function kitInstallToTell(status: { kitInstall?: KitInstall }, told: readonly string[]): KitInstall | undefined {
    const run = status.kitInstall;
    if (run === undefined || typeof run.at !== 'string' || told.includes(run.at)) return undefined;
    return run.outcome === 'installed' || run.outcome === 'failed' ? run : undefined;
}
