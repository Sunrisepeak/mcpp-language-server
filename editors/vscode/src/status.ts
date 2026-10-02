// The one piece of user interface this extension keeps visible: a language
// status item for C++ files, driven by the server's cxxModules/status notification.

import * as vscode from 'vscode';
import { cacheSegment, clampMaxLength, combineTier, fits, Tier, CxxCacheStatus } from './cacheSegment';
import type { OnlineRun } from './downloadAsk';
import { offersCacheReset, RESET_CACHE_COMMAND } from './cacheReset';
import { cachedCacheDetail, OPEN_CACHE_HUB_COMMAND, SWEEP_WORKSPACE_CACHE_COMMAND, COPY_AGENT_PROMPT_COMMAND } from './cacheSweep';
import { TURN_ON_COMMAND } from './enable';
import { stateTexts } from './statusText';
import { cardMarkdown } from './tooltipCard';
import type { ModuleIssueLike } from './unrecoverable';

export type ModuleState = 'starting' | 'loading' | 'preparing' | 'ready' | 'degraded' | 'error';

export interface SemanticProfile {
    kind: 'build-toolchain' | 'semantic-kit';
    compiler?: string;
    stdlib: string;
    target: string;
}

export interface IssueCommand {
    title: string;
    command: string;
    arguments?: unknown[];
}

export interface ModuleIssue extends ModuleIssueLike {
    code: string;
    message: string;
    command?: IssueCommand;
    // 0.0.7: the absolute path of a diagnostic bundle the server wrote by itself for an issue it
    // cannot recover from (see unrecoverable.ts).
    bundle?: string;
    // S3 extension (design 2026-09-25 §6): "code" is the user's own text being wrong, shown as a
    // diagnostic instead, and never colours the status. Absent means non-code, for a server from
    // before this field existed; see statusText.ts.
    category?: 'code' | 'engine' | 'environment' | 'project';
}

export interface CxxModulesStatus {
    state: ModuleState;
    project: {
        root: string;
        source: 'mcpp' | 'cmake' | 'xmake' | 'meson' | 'build-database' | 'compile-commands' | 'inferred';
        level?: number;
        // How the project was described (README L1..L4; S3-4-8, S3-4-9): 1 a build database, 2
        // CMake's own database, 3 a compile_commands.json, 4 sources only. Shown as
        // `L<tier>` instead of `level`, which is S1's own document-conformance number and would
        // read as the same thing to someone who does not know the difference.
        tier?: number;
    };
    profile: SemanticProfile;
    // The core semantic engine; "none" when a root has none (S3 4, S3-4-6).
    engine: { name: string; version: string };
    // Every engine serving the root, mcppls's own module engine included (S3-4-5).
    engines?: { name: string; version: string; role: string; state: string }[];
    progress?: { done: number; total: number };
    issues?: ModuleIssue[];
    // Facts worth showing that reduce no feature, e.g. a Visual Studio without the std module (S3 4).
    notices?: ModuleIssue[];
    // D-5 (plan 0.0.9): how the last fetch the person asked for ended (downloadPrompt.ts tells it once).
    onlineRun?: OnlineRun;
    // 0.0.10 plan C-13.1 (S3-4-29): the cache's coarse numbers, present when the client declared
    // `status: true`; the detail lives behind `cxxModules/cache`.
    cache?: CxxCacheStatus;
}

// What the status bar shows for each state.
//
// VS Code accepts only two status bar BACKGROUNDS — `statusBarItem.errorBackground` and
// `statusBarItem.warningBackground`. Anything else is ignored silently, so "ready" cannot have a
// green background however much it would suit it. The foreground takes any ThemeColor, so ready is
// a green tick on the normal background: visible, and not a coloured bar competing for attention
// with the states that have earned it.
function barFor(state: ModuleState | 'starting', detail: string | undefined):
        { text: string; background?: vscode.ThemeColor; foreground?: vscode.ThemeColor } {
    const label = (icon: string) => `${icon} mcppls${detail ? ` · ${detail}` : ''}`;
    switch (state) {
        case 'starting':
        case 'loading':
        case 'preparing':
            return { text: label('$(sync~spin)') };
        case 'ready':
            return { text: label('$(check)'), foreground: new vscode.ThemeColor('mcppls.statusReadyForeground') };
        case 'degraded':
            return { text: label('$(warning)'), background: new vscode.ThemeColor('statusBarItem.warningBackground') };
        case 'error':
            return { text: label('$(error)'), background: new vscode.ThemeColor('statusBarItem.errorBackground') };
    }
}

// While modules are being prepared the bar pulses, so a cold start reads as "working" from
// peripheral vision rather than only when someone stops to read the numbers.
//
// It pulses the FOREGROUND, not the text. Blinking the label itself is the obvious reading of the
// request, and the wrong one here: `Preparing modules 9/646` and `Preparing modules 128/646` are
// different widths, so a label that disappears and returns shifts every status bar item to its
// right, twice a second, for the whole cold start. Colour alternates in place -- no reflow, no
// neighbours moving, and it degrades to "steady text" rather than "garbled" on a theme that
// resolves the colour to the default.
const PULSE_MS = 900;

const SHOW_LOGS: vscode.Command = { title: 'Show Logs', command: 'mcppls.showLogs' };
// Issue #23 fix plan F18: what a limited state without a fix of its own offers is the bundle a report of the
// problem needs, which also carries the report.
const EXPORT_BUNDLE: vscode.Command = { title: 'Export Diagnostic Bundle', command: 'mcppls.exportDiagnosticBundle' };
const RESET_CACHE: vscode.Command = { title: 'Reset This Workspace\'s Cache', command: RESET_CACHE_COMMAND };
const TURN_ON: vscode.Command = { title: 'Turn On in This Workspace', command: TURN_ON_COMMAND };
const RESTART: vscode.Command = { title: 'Restart', command: 'mcppls.restartServer' };
const BUSY_STATES: readonly ModuleState[] = ['starting', 'loading', 'preparing'];

interface Waiter {
    states: readonly ModuleState[];
    resolve: (status: CxxModulesStatus) => void;
    reject: (error: Error) => void;
    timer: NodeJS.Timeout;
}

// The module state's own tier: what S1 colours the item with on its own (C-13.2's S1 column).
function moduleTierOf(state: ModuleState | 'starting'): Tier {
    if (state === 'error') return 2;
    if (state === 'degraded') return 1;
    return 0;
}

export function describeProfile(profile: SemanticProfile | undefined): string {
    if (!profile) {
        return '';
    }
    return profile.compiler && profile.compiler.length > 0 ? profile.compiler : profile.stdlib ?? '';
}

export class StatusController implements vscode.Disposable {
    private readonly item: vscode.LanguageStatusItem;
    // A LanguageStatusItem lives behind the `{}` icon: a person has to go looking for it, and
    // during a cold start the thing they want to know is exactly the thing they are not looking
    // for. The status bar item is always in view and carries the same state, coloured when it is
    // not "everything is fine" (cold-start plan 4.2).
    private readonly bar: vscode.StatusBarItem;
    private current: CxxModulesStatus | undefined;
    private failure: string | undefined;
    private readonly waiters = new Set<Waiter>();
    private pulseTimer: NodeJS.Timeout | undefined;
    private pulseLit = false;

    constructor() {
        this.item = vscode.languages.createLanguageStatusItem('mcppls.status', { language: 'cpp' });
        this.item.name = 'C++ Modules';
        this.bar = vscode.window.createStatusBarItem('mcppls.statusBar', vscode.StatusBarAlignment.Left, 50);
        this.bar.name = 'C++ Modules Language Server';
        // C-13.2 (plan 2026-10-03): the item opens the cache hub -- the one menu with the sweep, the
        // maintenance and the open-source actions. `showOff` keeps its one-click way back (below).
        this.bar.command = OPEN_CACHE_HUB_COMMAND;
        this.bar.show();
        this.showStarting();
    }

    // `mcppls.enable` is false for this workspace: the server is not started at all, and the one
    // status bar item is the way back (its command writes the setting and the extension starts it).
    showOff(): void {
        this.failure = undefined;
        this.current = undefined;
        this.setPulsing(false);
        this.item.text = 'C++ Modules';
        this.item.detail = 'Off in this workspace';
        this.item.busy = false;
        this.item.severity = vscode.LanguageStatusSeverity.Information;
        this.item.command = TURN_ON;
        this.bar.text = '$(circle-slash) C++ Modules: off in this workspace';
        this.bar.backgroundColor = undefined;
        this.bar.color = undefined;
        this.bar.tooltip = 'mcppls is turned off in this workspace (mcppls.enable). Click to turn it on.';
        this.bar.command = TURN_ON_COMMAND;
        for (const waiter of [...this.waiters]) {
            this.settle(waiter);
            waiter.reject(new Error('mcppls is turned off in this workspace.'));
        }
    }

    showStarting(detail = 'Starting'): void {
        this.bar.command = OPEN_CACHE_HUB_COMMAND;
        this.failure = undefined;
        this.current = undefined;
        this.item.text = 'C++ Modules';
        this.item.detail = detail;
        this.item.busy = true;
        this.item.severity = vscode.LanguageStatusSeverity.Information;
        this.item.command = SHOW_LOGS;
        this.paint('starting', detail);
    }

    // The status bar half of the same state. `detail` is the short label shown next to the icon;
    // `tooltipDetail` (the full text, when it differs -- only `degraded` shortens anything, see
    // statusText.ts) is what the tooltip shows, defaulting to `detail` when there is nothing fuller.
    private paint(state: ModuleState | 'starting', detail: string | undefined, tooltipDetail: string | undefined = detail): void {
        const { text, foreground } = barFor(state, detail);
        const busy = BUSY_STATES.includes(state as ModuleState);
        // C-13.2 (0.0.10 plan, D10/D12): the cache is a SEGMENT of this one item -- appended when the
        // budget fits it and (`auto`) only when it has something to say. The module text is never
        // shortened or dropped for the cache's sake; what does not fit lives in the hover card.
        const configuration = vscode.workspace.getConfiguration('mcppls');
        const maxLength = clampMaxLength(configuration.get('statusBar.maxLength'));
        const mode = configuration.get<string>('cache.showInStatusBar', 'auto');
        const segment = cacheSegment(this.current?.cache);
        const visible = segment !== undefined && (mode === 'always' || (mode === 'auto' && segment.tier >= 1));
        let whole = text;
        let cacheTier: Tier | undefined;
        if (segment && visible && fits(maxLength, whole, ` ${segment.icon} ${segment.text}`)) {
            whole = `${text} ${segment.icon} ${segment.text}`;
            cacheTier = segment.tier;
        }
        // The whole item wears the worst tier of its segments (the cache never hides the module's
        // own colour, it can only add to it): D10's "max(S1, S2)".
        const tier = combineTier(moduleTierOf(state), cacheTier);
        this.bar.text = whole;
        this.bar.backgroundColor = tier >= 2 ? new vscode.ThemeColor('statusBarItem.errorBackground')
            : tier === 1 ? new vscode.ThemeColor('statusBarItem.warningBackground')
                : undefined;
        // A busy repaint lands on every progress notification. Reading the pulse's current phase
        // here, rather than resetting the colour, keeps one steady rhythm across those repaints
        // instead of restarting the cycle a few times a second.
        this.bar.color = busy ? this.pulseColor() : foreground;
        this.bar.tooltip = this.cardTooltip(detail, tooltipDetail);
        this.setPulsing(busy);
    }

    private moduleLine(detail: string | undefined, tooltipDetail: string | undefined): string {
        return `C++ Modules${tooltipDetail || detail ? ` — ${tooltipDetail ?? detail}` : ''}`;
    }

    // The hover card (C-13.3): read-only markdown, command links through the same trusted-command
    // mechanism the reset link in `update` already uses. What it shows comes from the coarse status
    // numbers, plus the last detail the hub or a sweep fetched.
    private cardTooltip(detail: string | undefined, tooltipDetail: string | undefined): vscode.MarkdownString | string {
        const coarse = this.current?.cache;
        if (!coarse) {
            return tooltipDetail || detail ? `mcppls — ${tooltipDetail ?? detail}` : 'mcppls';
        }
        const markdown = new vscode.MarkdownString(cardMarkdown(this.moduleLine(detail, tooltipDetail), {
            coarse,
            detail: cachedCacheDetail(),
            withCommands: true,
            sweepCommand: SWEEP_WORKSPACE_CACHE_COMMAND,
            copyPromptCommand: COPY_AGENT_PROMPT_COMMAND,
        }), true);
        markdown.isTrusted = { enabledCommands: [SWEEP_WORKSPACE_CACHE_COMMAND, COPY_AGENT_PROMPT_COMMAND] };
        return markdown;
    }

    private pulseColor(): vscode.ThemeColor | undefined {
        return this.pulseLit ? new vscode.ThemeColor('mcppls.statusPreparingForeground') : undefined;
    }

    // Idempotent: starting an already-running pulse is a no-op, so the repaints above cannot
    // stack timers, and every exit from a busy state stops exactly one.
    private setPulsing(active: boolean): void {
        if (!active) {
            if (this.pulseTimer !== undefined) {
                clearInterval(this.pulseTimer);
                this.pulseTimer = undefined;
            }
            this.pulseLit = false;
            return;
        }
        if (this.pulseTimer !== undefined) {
            return;
        }
        this.pulseTimer = setInterval(() => {
            this.pulseLit = !this.pulseLit;
            this.bar.color = this.pulseColor();
        }, PULSE_MS);
    }

    // The server is up but has not described itself; it does not implement cxxModules/status.
    showRunning(): void {
        this.item.text = 'C++ Modules';
        this.item.detail = 'Running';
        this.item.busy = false;
        this.item.severity = vscode.LanguageStatusSeverity.Information;
        this.item.command = SHOW_LOGS;
        this.paint('ready', 'Running');
    }

    // A failure seen by the extension itself: no payload, or a server that will not stay up.
    showFailure(message: string, offerRestart = false): void {
        this.failure = message;
        this.item.text = 'C++ Modules';
        this.item.detail = message;
        this.item.busy = false;
        this.item.severity = vscode.LanguageStatusSeverity.Error;
        this.item.command = offerRestart ? RESTART : SHOW_LOGS;
        this.bar.command = OPEN_CACHE_HUB_COMMAND;   // the hub carries 重启服务端 and 打开日志
        this.paint('error', message);
        for (const waiter of [...this.waiters]) {
            this.settle(waiter);
            waiter.reject(new Error(message));
        }
    }

    // Called with every status the server sends, after the item shows it (downloadPrompt.ts listens).
    private readonly listeners: ((status: CxxModulesStatus) => void)[] = [];
    onUpdate(listener: (status: CxxModulesStatus) => void): void {
        this.listeners.push(listener);
    }

    update(status: CxxModulesStatus): void {
        this.current = status;
        queueMicrotask(() => {
            for (const listener of this.listeners) {
                listener(status);
            }
        });
        this.failure = undefined;
        const label = describeProfile(status.profile);
        this.item.text = label.length > 0 ? `C++ Modules · ${label}` : 'C++ Modules';

        const details: string[] = [];
        const texts = stateTexts(status);
        if (texts.full) {
            // The full, unshortened text; see statusText.ts for why only `degraded` differs from `short`.
            details.push(texts.full);
        }
        if (status.project) {
            const tier = status.project.tier;
            details.push(tier !== undefined ? `${status.project.source} · L${tier}` : status.project.source);
        }
        if (status.engines && status.engines.length > 0) {
            details.push(status.engines.map((engine) => `${engine.name} ${engine.version}`.trim()).join(' + '));
        } else if (status.engine) {
            details.push(`${status.engine.name} ${status.engine.version}`.trim());
        }
        const issues = status.issues ?? [];
        if (!texts.full && status.notices && status.notices.length > 0) {
            // Shown in the item's hover only: a notice changes neither the state nor the severity.
            // Only when there is no issue text already in `details` above -- same precedence as before.
            details.push(status.notices[0].message);
        }
        this.item.detail = details.join(' · ');
        this.item.busy = BUSY_STATES.includes(status.state);
        this.item.severity = status.state === 'error'
            ? vscode.LanguageStatusSeverity.Error
            : status.state === 'degraded'
                ? vscode.LanguageStatusSeverity.Warning
                : vscode.LanguageStatusSeverity.Information;
        const withCommand = issues.find((issue) => issue.command !== undefined);
        // A limited state without a fix of its own offers what a bug report needs (robustness design O4).
        this.item.command = withCommand?.command
            ? { title: withCommand.command.title, command: withCommand.command.command, arguments: withCommand.command.arguments }
            : offersCacheReset(issues) ? RESET_CACHE
                : status.state === 'degraded' || status.state === 'error' ? EXPORT_BUNDLE : SHOW_LOGS;

        // The status bar says the one thing that matters now, shortened when there is a fuller
        // version in the tooltip; the item behind `{}` keeps the rest.
        const shortDetail = texts.short ?? (describeProfile(status.profile) || undefined);
        this.paint(status.state, shortDetail, texts.full ?? shortDetail);
        if (offersCacheReset(issues)) {
            // A tooltip link next to the item's own click action, so the reset is offered
            // alongside whatever the issue itself offers (0.0.7 plan C-1).
            const tooltip = new vscode.MarkdownString(undefined, true);
            tooltip.isTrusted = { enabledCommands: [RESET_CACHE_COMMAND] };
            tooltip.appendText(`mcppls — ${texts.full ?? shortDetail ?? ''}\n\n`);
            tooltip.appendMarkdown(`[${RESET_CACHE.title}](command:${RESET_CACHE_COMMAND})`);
            this.bar.tooltip = tooltip;
        }

        for (const waiter of [...this.waiters]) {
            if (waiter.states.includes(status.state)) {
                this.settle(waiter);
                waiter.resolve(status);
            } else if (status.state === 'error') {
                this.settle(waiter);
                waiter.reject(new Error(`The server reported the error state: ${issues.map((issue) => issue.message).join('; ')}`));
            }
        }
    }

    // The status bar item's text, for the end-to-end tests.
    barText(): string {
        return this.bar.text;
    }

    lastStatus(): CxxModulesStatus | undefined {
        return this.current;
    }

    waitForState(state: ModuleState | readonly ModuleState[], timeoutMs: number): Promise<CxxModulesStatus> {
        const states: readonly ModuleState[] = typeof state === 'string' ? [state] : state;
        if (this.current && states.includes(this.current.state)) {
            return Promise.resolve(this.current);
        }
        if (this.failure) {
            return Promise.reject(new Error(this.failure));
        }
        return new Promise<CxxModulesStatus>((resolve, reject) => {
            const waiter: Waiter = {
                states,
                resolve,
                reject,
                timer: setTimeout(() => {
                    this.waiters.delete(waiter);
                    reject(new Error(`Timed out after ${timeoutMs} ms waiting for ${states.join(' or ')}; last status: ${JSON.stringify(this.current)}`));
                }, timeoutMs),
            };
            this.waiters.add(waiter);
        });
    }

    dispose(): void {
        this.setPulsing(false);
        for (const waiter of [...this.waiters]) {
            this.settle(waiter);
            waiter.reject(new Error('The extension was deactivated.'));
        }
        this.item.dispose();
        this.bar.dispose();
    }

    private settle(waiter: Waiter): void {
        clearTimeout(waiter.timer);
        this.waiters.delete(waiter);
    }
}
