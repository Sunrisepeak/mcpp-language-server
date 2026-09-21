// The one piece of user interface this extension keeps visible: a language
// status item for C++ files, driven by the server's cxxModules/status notification.

import * as vscode from 'vscode';

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

export interface ModuleIssue {
    code: string;
    message: string;
    command?: IssueCommand;
}

export interface CxxModulesStatus {
    state: ModuleState;
    project: {
        root: string;
        source: 'mcpp' | 'cmake' | 'build-database' | 'compile-commands' | 'inferred';
        level?: number;
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
const COLLECT_REPORT: vscode.Command = { title: 'Collect Report', command: 'mcppls.collectReport' };
const RESTART: vscode.Command = { title: 'Restart', command: 'mcppls.restartServer' };
const BUSY_STATES: readonly ModuleState[] = ['starting', 'loading', 'preparing'];

interface Waiter {
    states: readonly ModuleState[];
    resolve: (status: CxxModulesStatus) => void;
    reject: (error: Error) => void;
    timer: NodeJS.Timeout;
}

export function describeProfile(profile: SemanticProfile | undefined): string {
    if (!profile) {
        return '';
    }
    return profile.compiler && profile.compiler.length > 0 ? profile.compiler : profile.stdlib ?? '';
}

function stateText(status: CxxModulesStatus): string | undefined {
    switch (status.state) {
        case 'starting':
            return 'Starting';
        case 'loading':
            return 'Loading the project';
        case 'preparing':
            return status.progress && status.progress.total > 0
                ? `Preparing modules ${status.progress.done}/${status.progress.total}`
                : 'Preparing modules';
        case 'ready':
            return undefined;
        case 'degraded':
            return 'Some features are limited';
        case 'error':
            return 'Only module-level features are available';
    }
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
        this.bar.name = 'C++ Modules (mcppls)';
        this.bar.command = 'mcppls.showLogs';
        this.bar.show();
        this.showStarting();
    }

    showStarting(detail = 'Starting'): void {
        this.failure = undefined;
        this.current = undefined;
        this.item.text = 'C++ Modules';
        this.item.detail = detail;
        this.item.busy = true;
        this.item.severity = vscode.LanguageStatusSeverity.Information;
        this.item.command = SHOW_LOGS;
        this.paint('starting', detail);
    }

    // The status bar half of the same state.
    private paint(state: ModuleState | 'starting', detail: string | undefined): void {
        const { text, background, foreground } = barFor(state, detail);
        const busy = BUSY_STATES.includes(state as ModuleState);
        this.bar.text = text;
        this.bar.backgroundColor = background;
        // A busy repaint lands on every progress notification. Reading the pulse's current phase
        // here, rather than resetting the colour, keeps one steady rhythm across those repaints
        // instead of restarting the cycle a few times a second.
        this.bar.color = busy ? this.pulseColor() : foreground;
        this.bar.tooltip = detail ? `mcppls — ${detail}` : 'mcppls';
        this.setPulsing(busy);
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
        this.paint('error', message);
        for (const waiter of [...this.waiters]) {
            this.settle(waiter);
            waiter.reject(new Error(message));
        }
    }

    update(status: CxxModulesStatus): void {
        this.current = status;
        this.failure = undefined;
        const label = describeProfile(status.profile);
        this.item.text = label.length > 0 ? `C++ Modules · ${label}` : 'C++ Modules';

        const details: string[] = [];
        const state = stateText(status);
        if (state) {
            details.push(state);
        }
        if (status.project) {
            details.push(status.project.level !== undefined
                ? `${status.project.source} · level ${status.project.level}`
                : status.project.source);
        }
        if (status.engines && status.engines.length > 0) {
            details.push(status.engines.map((engine) => `${engine.name} ${engine.version}`.trim()).join(' + '));
        } else if (status.engine) {
            details.push(`${status.engine.name} ${status.engine.version}`.trim());
        }
        const issues = status.issues ?? [];
        if ((status.state === 'degraded' || status.state === 'error') && issues.length > 0) {
            details.push(issues[0].message);
        } else if (status.notices && status.notices.length > 0) {
            // Shown in the item's hover only: a notice changes neither the state nor the severity.
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
        // A limited state without a fix of its own offers the report a bug report needs (robustness design O4).
        this.item.command = withCommand?.command
            ? { title: withCommand.command.title, command: withCommand.command.command, arguments: withCommand.command.arguments }
            : status.state === 'degraded' || status.state === 'error' ? COLLECT_REPORT : SHOW_LOGS;

        // The status bar says the one thing that matters now; the item behind `{}` keeps the rest.
        this.paint(status.state, stateText(status) ?? (describeProfile(status.profile) || undefined));

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
