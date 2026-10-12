// The QuickPick hub's items (0.0.10 plan C-13.3, D20; v2 2026-10-03 UI-8..UI-10): four separator
// groups (概览 / 清理 / 诊断 / 反馈), every entry opens with its codicon, and -- the fix for the
// drill-down that never opened -- each entry SAYS what accepting it does in its `behavior`, so the
// view dispatches on data, never on the label's icon text. Pure: the view (cacheHubView.ts) only
// draws this.
import { CacheDetail, sizeText } from './cacheSegment';
import { COPY_AGENT_PROMPT_COMMAND, REVEAL_CACHE_DIRECTORY_COMMAND, SWEEP_WORKSPACE_CACHE_COMMAND } from './cacheSweep';
import type { CxxModulesStatus } from './status';
import { t } from './strings';

/** The command an overview entry runs to copy the versions (0.0.13 W7). */
export const COPY_ABOUT_COMMAND = 'mcppls.copyAbout';

/** What accepting an entry does. `sweep` runs the sweep flow; `detail` swaps in a drill-down list. */
export type HubBehavior = 'sweep' | 'command' | 'detail' | 'refresh';

export interface HubAction {
    command: string;
    /** Arguments the command receives; a sweep carries the category list, a link the URL. */
    arguments?: unknown[];
    /** The command opens a browser (only these go through `openExternal`, and only on click). */
    external?: boolean;
}

export interface HubEntry {
    icon: string;
    label: string;
    description?: string;
    behavior: HubBehavior;
    /** `behavior: 'command'`: what accepting the entry runs. */
    action?: HubAction;
    /** `behavior: 'detail'`: which drill-down list takes over. */
    detail?: 'modules' | 'directories';
    /** The `$(eye)` dry-run button, on the sweep entry only (D20). */
    buttonTitle?: string;
}

export type HubItem = { kind: 'separator'; label: string } | ({ kind: 'entry' } & HubEntry);

export interface HubCapabilities {
    /** The server advertises `mcppls.sweepCache`; otherwise the whole 清理 group's sweep stays out. */
    canSweep: boolean;
}

/** The main entry, with the `$(eye)` dry-run button the view hangs on it (D20). */
export function sweepEntry(receipt?: string): HubEntry {
    return {
        icon: '$(clear-all)',
        label: t('Sweep the cache (no restart, no rebuild)'),
        description: receipt ?? t('Dry run? Use the eye button'),
        buttonTitle: t('Dry run: see what would go, remove nothing'),
        behavior: 'sweep',
        action: { command: SWEEP_WORKSPACE_CACHE_COMMAND, arguments: [{ dryRun: false }] },
    };
}

function percentOf(detail: CacheDetail): number {
    return detail.limits.perWorkspace > 0 ? Math.round((detail.bytes / detail.limits.perWorkspace) * 100) : 0;
}

function uptimeText(startedAt: string | undefined, now: number): string | undefined {
    const started = startedAt ? Date.parse(startedAt) : NaN;
    if (Number.isNaN(started)) return undefined;
    const minutes = Math.max(0, Math.round((now - started) / 60000));
    return minutes < 60 ? t('up {0} min', minutes) : t('up {0} h {1} min', Math.floor(minutes / 60), minutes % 60);
}

/** The versions as one line of text, for an issue or a chat: no paths. */
export function aboutText(status: CxxModulesStatus): string {
    const about = status.about;
    if (!about) return `mcppls · ${status.engine.name} ${status.engine.version}`;
    const parts = [`mcppls ${about.server}`];
    if (about.engine && about.engine.name !== 'none') {
        parts.push(`engine ${about.engine.name} ${about.engine.version ?? ''}${about.engine.build && about.engine.build !== about.engine.version ? ` (${about.engine.build})` : ''}`.trim());
    }
    if (about.mcpp !== undefined) parts.push(`mcpp ${about.mcpp || t('(version unknown)')}`);
    if (about.extension) parts.push(`extension ${about.extension}`);
    if (about.editor) parts.push(`${about.editor.name} ${about.editor.version}`);
    parts.push(about.platform);
    return parts.join(' · ');
}

/**
 * The overview's first lines (0.0.13 W7): what is running and how it is doing, so a person can
 * confirm the version and the state without opening a log. A version mismatch comes first.
 */
export function runningEntries(status: CxxModulesStatus | undefined, now: number = Date.now()): HubEntry[] {
    if (!status) return [];
    const entries: HubEntry[] = [];
    const about = status.about;
    if (about?.mismatch) {
        entries.push({ icon: '$(warning)', label: t('Version mismatch'), description: about.mismatch, behavior: 'command',
                       action: { command: COPY_ABOUT_COMMAND, arguments: [aboutText(status)] } });
    }
    entries.push({ icon: '$(versions)', label: aboutText(status), description: t('click to copy'), behavior: 'command',
                   action: { command: COPY_ABOUT_COMMAND, arguments: [aboutText(status)] } });
    const state: string[] = [status.state];
    if (status.progress && status.progress.total > 0) state.push(t('modules {0}/{1}', status.progress.done, status.progress.total));
    const uptime = uptimeText(about?.startedAt, now);
    if (uptime) state.push(uptime);
    if (about?.engine?.restarts !== undefined) state.push(t('restarts {0}', about.engine.restarts));
    const problems = (status.issues ?? []).length;
    if (problems > 0) state.push(t('{0} issues', problems));
    entries.push({ icon: '$(pulse)', label: t('Running'), description: state.join(' · '), behavior: 'command', action: { command: 'mcppls.showLogs' } });
    // 0.0.14 E1: on Windows, a cache off a Dev Drive is scanned by Microsoft Defender as clangd writes and reads it.
    if (about?.cacheDevDrive === false) {
        entries.push({ icon: '$(shield)', label: t('Cache is not on a Dev Drive'),
                       description: t('Defender scans module files as they are written and read; the log says what helps'),
                       behavior: 'command', action: { command: 'mcppls.showLogs' } });
    }
    return entries;
}

/** All entries of the hub, in display order. Engine names never appear (D17), except in the versions line. */
export function hubItems(detail: CacheDetail, caps: HubCapabilities, receipt?: string, status?: CxxModulesStatus): HubItem[] {
    const items: HubItem[] = [];
    items.push({ kind: 'separator', label: t('Overview') });
    for (const entry of runningEntries(status)) items.push({ kind: 'entry', ...entry });
    items.push({
        kind: 'entry',
        icon: '$(database)',
        label: t('Cache in use'),
        description: `${sizeText(detail.bytes)} / ${sizeText(detail.limits.perWorkspace)} · ${percentOf(detail)}%`
            + (detail.limits.over ? ` · ${t('over budget')}` : ''),
        behavior: 'refresh',
    });
    const largest = detail.largest ?? [];
    items.push({
        kind: 'entry',
        icon: '$(chevron-right)',
        label: t('Details: largest modules and directories'),
        description: largest.length > 0 ? t('{0} cached modules', largest.length) : undefined,
        behavior: 'detail',
        detail: 'modules',
    });
    if (detail.lastSweep && detail.lastSweep.at > 0) {
        const age = Math.max(1, Math.round((Date.now() - detail.lastSweep.at) / 1000));
        items.push({
            kind: 'entry',
            icon: '$(history)',
            label: t('Last sweep'),
            description: t('freed {0} ({1} files) · {2} ago', sizeText(detail.lastSweep.freedBytes), detail.lastSweep.files,
                           age < 60 ? t('{0} s ago', age) : t('{0} min ago', Math.round(age / 60)))
                + (detail.lastSweep.failed ? ` · ${t('{0} failed to delete', detail.lastSweep.failed)}` : ''),
            behavior: 'refresh',
        });
    }
    items.push({ kind: 'separator', label: t('Clean') });
    if (caps.canSweep) {
        items.push({ kind: 'entry', ...sweepEntry(receipt) });
    }
    items.push({ kind: 'entry', icon: '$(trash)', label: t('Reset the cache…'), description: t('rebuilds the modules'),
                 behavior: 'command', action: { command: 'mcppls.resetWorkspaceCache' } });
    items.push({ kind: 'separator', label: t('Diagnostics') });
    items.push({ kind: 'entry', icon: '$(debug-restart)', label: t('Restart the engine'), behavior: 'command',
                 action: { command: 'mcppls.restartClangd' } });
    items.push({ kind: 'entry', icon: '$(refresh)', label: t('Restart the server'), behavior: 'command',
                 action: { command: 'mcppls.restartServer' } });
    items.push({ kind: 'entry', icon: '$(file-zip)', label: t('Capture a diagnostic bundle'), description: t('with the cache report'),
                 behavior: 'command', action: { command: 'mcppls.exportDiagnosticBundle' } });
    items.push({ kind: 'entry', icon: '$(output)', label: t('Open the logs'), behavior: 'command', action: { command: 'mcppls.showLogs' } });
    items.push({ kind: 'entry', icon: '$(folder-opened)', label: t('Open a directory…'), description: t('cache · logs · bundles'),
                 behavior: 'detail', detail: 'directories' });
    items.push({ kind: 'separator', label: t('Feedback') });
    items.push({
        kind: 'entry',
        icon: '$(copy)',
        label: t('Copy the Agent Troubleshooting Prompt'),
        description: t('for a local AI agent: read-only checks, a report back -- logs never leave this machine'),
        behavior: 'command',
        action: { command: COPY_AGENT_PROMPT_COMMAND },
    });
    items.push({ kind: 'entry', icon: '$(github)', label: t('New issue…'), description: t('prefilled with version and environment'),
                 behavior: 'command', action: { command: 'mcppls.newCacheIssue' } });
    items.push({ kind: 'entry', icon: '$(repo)', label: t('Open the repository'), behavior: 'command', action: { command: 'mcppls.openRepository' } });
    items.push({ kind: 'entry', icon: '$(book)', label: t('Open the documentation'), behavior: 'command', action: { command: 'mcppls.openDocumentation' } });
    items.push({ kind: 'entry', icon: '$(gear)', label: t('Open the cache settings'), behavior: 'command', action: { command: 'mcppls.openCacheSettings' } });
    return items;
}

/** The largest-modules drill-down (D20): one Esc -- or the back button -- returns to the hub. */
export function drillDownItems(detail: CacheDetail): HubItem[] {
    const items: HubItem[] = [{ kind: 'separator', label: t('Largest modules') }];
    const largest = detail.largest ?? [];
    if (largest.length === 0) {
        items.push({ kind: 'entry', icon: '$(circle-slash)', label: t('No cached modules yet'), behavior: 'refresh' });
    }
    for (const module of largest.slice(0, 5)) {
        items.push({
            kind: 'entry',
            icon: '$(file-binary)',
            label: module.module,
            description: `${sizeText(module.bytes)} · ${t('{0} copies', module.copies)}`,
            behavior: 'refresh',
        });
    }
    items.push({ kind: 'separator', label: t('Feedback') });
    items.push({
        kind: 'entry',
        icon: '$(copy)',
        label: t('Copy the issue draft prompt'),
        description: t('the agent turns the findings into a draft, for you to read first'),
        behavior: 'command',
        action: { command: 'mcppls.copyIssuePrompt' },
    });
    return items;
}

/** The three directories a report can name (UI-8): the module cache, the logs, the bundles. */
export function directoryItems(detail: CacheDetail): HubItem[] {
    const one = (icon: string, label: string, which: string): HubItem => ({
        kind: 'entry', icon, label, description: t('reveal in the file manager'), behavior: 'command',
        action: { command: REVEAL_CACHE_DIRECTORY_COMMAND, arguments: [which] },
    });
    return [
        { kind: 'separator', label: t('Directories') },
        one('$(database)', t('Module cache ({0})', sizeText(detail.bytes)), 'cache'),
        one('$(output)', t('Logs'), 'logs'),
        one('$(file-zip)', t('Diagnostic bundles'), 'bundles'),
    ];
}

/** What the title says, engine-free (D17): the project, then the cache against its budget. */
export function hubTitle(detail: CacheDetail): string {
    const limit = detail.limits.perWorkspace;
    const percent = limit > 0 ? ` · ${Math.round((detail.bytes / limit) * 100)}%` : '';
    return `${detail.project.name} — ${sizeText(detail.bytes)} / ${sizeText(limit)}${percent}`;
}

/** The entry's visible label with its icon, the way the view draws it. */
export function entryLabel(entry: HubEntry): string {
    return `${entry.icon} ${entry.label}`;
}
