// The address of a bug report that is already filled in as far as the extension can fill it
// (0.0.7 plan: an unrecoverable error is one click from a report a maintainer can act on).
//
// GitHub issue forms take a query parameter per field, named after the field's `id` in the form
// (.github/ISSUE_TEMPLATE/bug_report.yml), and `title`, `template`. A test reads that file and
// compares the ids, so the two cannot drift apart. No `vscode` import: plain Node tests load it.

export const REPOSITORY = 'https://github.com/Sunrisepeak/mcpp-language-server';
export const BUG_REPORT_TEMPLATE = 'bug_report.yml';

// The form fields this module prefills; every one is an `id` in bug_report.yml.
export const PREFILLED_FIELD_IDS = ['version', 'editor', 'os', 'what-happened'] as const;
export type PrefilledFieldId = typeof PREFILLED_FIELD_IDS[number];

export interface IssueContext {
    code: string;
    // The issue's own message, or "stopped after crashing N times".
    message: string;
    extensionVersion?: string;
    serverVersion?: string;
    // vscode.env.appName and vscode.version.
    appName: string;
    editorVersion: string;
    // process.platform and process.arch.
    platform: string;
    arch: string;
    // Where the diagnostic bundle or crash report is on this machine, when there is one.
    bundlePath?: string;
    bundleKind?: 'bundle' | 'crash-report';
}

// A URL longer than this is refused by GitHub's servers with a 414 or silently cut, and a form that
// cannot open is worse than one with a shorter title.
export const MAX_URL_LENGTH = 6000;
const MAX_TITLE_MESSAGE = 80;

export function shortMessage(message: string, max = MAX_TITLE_MESSAGE): string {
    const oneLine = message.replace(/\s+/g, ' ').trim();
    if (oneLine.length <= max) return oneLine;
    const cut = oneLine.slice(0, max - 1);
    const space = cut.lastIndexOf(' ');
    return `${(space > max / 3 ? cut.slice(0, space) : cut).trimEnd()}…`;
}

export function osName(platform: string, arch: string): string {
    const names: Record<string, string> = { linux: 'Linux', darwin: 'macOS', win32: 'Windows' };
    return `${names[platform] ?? platform} (${arch})`;
}

export function issueFields(context: IssueContext): Record<PrefilledFieldId, string> {
    const versions = [
        context.extensionVersion ? `extension ${context.extensionVersion}` : undefined,
        context.serverVersion ? `server ${context.serverVersion}` : undefined,
    ].filter((part): part is string => part !== undefined);
    const where = context.bundlePath
        ? context.bundleKind === 'crash-report'
            ? `\n\nmcppls saved a crash report on this machine: ${context.bundlePath}\nPlease attach that folder (zipped) to this issue.`
            : `\n\nmcppls saved a diagnostic bundle on this machine: ${context.bundlePath}\nPlease attach that file to this issue.`
        : '\n\nNo diagnostic bundle is attached yet: run "C++ Modules: Export Diagnostic Bundle" and attach the file.';
    return {
        'version': versions.length > 0 ? versions.join(', ') : 'unknown',
        'editor': `${context.appName} ${context.editorVersion}`.trim(),
        'os': osName(context.platform, context.arch),
        'what-happened': `mcppls reported [${context.code}]: ${shortMessage(context.message, 300)}${where}`,
    };
}

export function issueTitle(code: string, message: string): string {
    return `[${code}] ${shortMessage(message)}`;
}

// 0.0.10 plan C-13.3 (D19): the same fields, opened by a person who is reporting on their own
// initiative rather than from a crash -- no `[code]` prefix in the title. Everything else
// (template, length bound, encoding) is `buildIssueUrl`'s own machinery.
export function feedbackIssueUrl(context: IssueContext): string {
    const fields = issueFields(context);
    const parameters: [string, string][] = [
        ['template', BUG_REPORT_TEMPLATE],
        ['title', shortMessage(context.message)],
        ...PREFILLED_FIELD_IDS.map((id) => [id, fields[id]] as [string, string]),
    ];
    const render = (list: [string, string][]): string =>
        `${REPOSITORY}/issues/new?${list.map(([key, value]) => `${key}=${encodeURIComponent(value)}`).join('&')}`;
    let url = render(parameters);
    if (url.length > MAX_URL_LENGTH) url = render(parameters.filter(([key]) => key !== 'what-happened'));
    if (url.length > MAX_URL_LENGTH) url = render(parameters.filter(([key]) => key !== 'what-happened' && key !== 'title'));
    return url;
}

export function buildIssueUrl(context: IssueContext): string {
    const parameters: [string, string][] = [
        ['template', BUG_REPORT_TEMPLATE],
        ['title', issueTitle(context.code, context.message)],
    ];
    const fields = issueFields(context);
    for (const id of PREFILLED_FIELD_IDS) {
        parameters.push([id, fields[id]]);
    }
    const render = (list: readonly [string, string][]): string =>
        `${REPOSITORY}/issues/new?${list.map(([key, value]) => `${key}=${encodeURIComponent(value)}`).join('&')}`;
    let url = render(parameters);
    // Too long: drop the free-text field first, then the title; the fixed fields are short.
    if (url.length > MAX_URL_LENGTH) {
        url = render(parameters.filter(([key]) => key !== 'what-happened'));
    }
    if (url.length > MAX_URL_LENGTH) {
        url = render(parameters.filter(([key]) => key !== 'what-happened' && key !== 'title'));
    }
    return url;
}
