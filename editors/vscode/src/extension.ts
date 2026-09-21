// C++ Modules for VS Code: a thin client for the mcppls language server.
//
// The extension starts the server from the bundled payload, advertises the
// cxxModules protocol extension, shows one language status item, and otherwise
// stays out of sight: no notifications, no status bar items, no walkthroughs.

import * as vscode from 'vscode';
import {
    ClientCapabilities,
    CloseAction,
    CloseHandlerResult,
    ErrorAction,
    Executable,
    FeatureState,
    LanguageClient,
    LanguageClientOptions,
    MessageType,
    RevealOutputChannelOn,
    ServerOptions,
    ShowMessageNotification,
    ShowMessageRequest,
    State,
    StaticFeature,
    TransportKind,
} from 'vscode-languageclient/node';
import { CommandLineToolsController, withInstallCommandFallback } from './commandLineTools';
import { registerCommands, reloadBuildDescription } from './commands';
import { checkConflicts, ConflictCheck } from './conflicts';
import { resolveLaunch } from './payload';
import { promptTestHarness, PromptKind } from './prompt';
import { CxxModulesStatus, ModuleState, StatusController } from './status';

// build description design 4.4: a value this extension does not know must not turn the network on.
function buildToolSetting(value: string | undefined): string {
    return value === 'online' || value === 'off' ? value : 'offline';
}

const CLIENT_ID = 'mcppls';
const CLIENT_NAME = 'C++ Modules';
const RESTART_WINDOW_MS = 3 * 60 * 1000;
const MAX_RESTARTS = 4;

export interface TestApi {
    waitForState(state: ModuleState | readonly ModuleState[], timeoutMs: number): Promise<CxxModulesStatus>;
    lastStatus(): CxxModulesStatus | undefined;
    // Calls the extension (or the bundled language client) made to VS Code
    // APIs that would put something in front of the user, while
    // MCPPLS_TEST=1; -1 when a given counter could not be installed. See
    // installUiCounters below for what each one means and, for
    // showTextDocumentCount specifically, why it is a snapshot rather than a
    // live count.
    notificationCount(): number;
    statusBarItemCount(): number;
    outputChannelShowCount(): number;
    webviewPanelCount(): number;
    showTextDocumentCount(): number;
    languageStatusItemCount(): number;
    conflictCheck(): Promise<ConflictCheck>;
    // Test-mode substitution for the two prompts the extension can show (see
    // src/prompt.ts). Safe to call at any time relative to when the
    // extension's own logic triggers that prompt.
    setPromptAnswer(kind: PromptKind, answer: string | undefined): void;
    promptShownCount(kind: PromptKind): number;
    // xcode-select --install is never actually spawned in test mode; this
    // counts how many times it would have been.
    commandLineToolsInstallCount(): number;
}

// Tells the server this client understands the cxxModules extension (S3).
class CxxModulesFeature implements StaticFeature {
    fillClientCapabilities(capabilities: ClientCapabilities): void {
        const experimental = (capabilities.experimental ?? {}) as Record<string, unknown>;
        experimental.cxxModules = { version: 1, status: true, graph: true, contexts: true };
        capabilities.experimental = experimental;
    }

    initialize(): void {
        // Nothing to register: the notification handler is installed on the client.
    }

    getState(): FeatureState {
        return { kind: 'static' };
    }

    clear(): void {
        // Stateless.
    }
}

function errorText(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

function messageTypeName(type: MessageType): string {
    switch (type) {
        case MessageType.Error:
            return 'error';
        case MessageType.Warning:
            return 'warning';
        case MessageType.Info:
            return 'info';
        default:
            return 'log';
    }
}

class ServerHost implements vscode.Disposable {
    private client: LanguageClient | undefined;
    private channel: vscode.LogOutputChannel | undefined;
    private restarts: number[] = [];
    private queue: Promise<void> = Promise.resolve();

    constructor(
        private readonly context: vscode.ExtensionContext,
        private readonly status: StatusController,
        private readonly commandLineTools: CommandLineToolsController,
        // Called every time the server actually (re)starts, i.e. once at
        // activation and once per mcppls.restartServer or a config-driven
        // restart -- but not for the language client's own crash-recovery
        // respawn, which never calls back into startNow(). Used to re-run
        // conflict detection at the same points a fresh cxxModules/status
        // naturally re-triggers the Command Line Tools check, so both
        // one-time questions are exercised the same way by a restart.
        private readonly onStarting: () => void,
    ) {}

    // Created on first use and kept across restarts, never revealed automatically. A log channel,
    // which is what vscode-languageclient 10 writes to (it has levels and timestamps of its own).
    output(): vscode.LogOutputChannel {
        if (!this.channel) {
            this.channel = vscode.window.createOutputChannel(CLIENT_NAME, { log: true });
        }
        return this.channel;
    }

    log(line: string): void {
        this.output().appendLine(`[${new Date().toLocaleTimeString()}] ${line}`);
    }

    runningClient(): LanguageClient | undefined {
        return this.client && this.client.state === State.Running ? this.client : undefined;
    }

    start(): Promise<void> {
        return this.enqueue(() => this.startNow());
    }

    stop(): Promise<void> {
        return this.enqueue(() => this.stopNow());
    }

    restart(): Promise<void> {
        return this.enqueue(async () => {
            this.restarts = [];
            await this.stopNow();
            await this.startNow();
        });
    }

    dispose(): void {
        // The client may still log while it stops, so the channel goes last.
        void this.stopNow().finally(() => {
            this.channel?.dispose();
            this.channel = undefined;
        });
    }

    private enqueue(operation: () => Promise<void>): Promise<void> {
        const next = this.queue.then(operation, operation);
        this.queue = next.catch(() => undefined);
        return next;
    }

    private async startNow(): Promise<void> {
        if (this.client) {
            return;
        }
        this.onStarting();
        const resolution = resolveLaunch(this.context.extensionPath);
        if (!resolution.ok) {
            this.log(resolution.reason);
            this.status.showFailure(resolution.reason);
            return;
        }
        const launch = resolution.launch;
        const configuration = vscode.workspace.getConfiguration('mcppls');

        const args = ['serve'];
        if (launch.payloadDir) {
            args.push('--payload', launch.payloadDir);
        }
        if (!vscode.workspace.isTrusted) {
            args.push('--untrusted');
        }
        const logLevel = process.env.MCPPLS_LOG_LEVEL
            ?? (configuration.get<string>('trace.server') === 'verbose' ? 'debug' : undefined);
        if (logLevel) {
            args.push('--log-level', logLevel);
        }

        const folder = vscode.workspace.workspaceFolders?.find((candidate) => candidate.uri.scheme === 'file');
        const executable: Executable = {
            command: launch.executable,
            args,
            transport: TransportKind.stdio,
            options: { cwd: folder?.uri.fsPath, env: { ...process.env } },
        };
        const serverOptions: ServerOptions = { run: executable, debug: executable };

        const compiler = (configuration.get<string>('compiler') ?? '').trim();
        const clientOptions: LanguageClientOptions = {
            documentSelector: [
                { scheme: 'file', language: 'cpp' },
                { scheme: 'file', language: 'c' },
            ],
            outputChannel: this.output(),
            revealOutputChannelOn: RevealOutputChannelOn.Never,
            initializationOptions: {
                compiler: compiler.length > 0 ? compiler : null,
                semanticKit: configuration.get<string>('semanticKit') === 'off' ? 'off' : 'auto',
                // overall design 5.6: the core semantic engine; mcppls's own module engine always runs.
                engine: configuration.get<string>('engine') === 'none' ? 'none' : 'clangd',
                // build description design 4.4: how the user's build tool may be run.
                buildTool: buildToolSetting(configuration.get<string>('buildTool')),
                // build description design 4.3: which environment it is run in.
                toolEnvironment: configuration.get<string>('toolEnvironment') === 'editor' ? 'editor' : 'auto',
                // This extension finds other C/C++ language servers itself (mcppls.detectConflicts)
                // and offers, once, to turn their language features off. Saying so keeps the server
                // from also explaining it: a server cannot see its siblings through LSP, so it tells
                // clients that arbitrate nothing — which is every editor but this one.
                conflictArbitration: 'client',
            },
            errorHandler: {
                error: () => ({ action: ErrorAction.Continue, handled: true }),
                closed: () => this.onClosed(),
            },
            initializationFailedHandler: (error) => {
                const reason = `The language server failed to initialize: ${errorText(error)}`;
                this.log(reason);
                this.status.showFailure(reason, true);
                return false;
            },
        };

        const client = new LanguageClient(CLIENT_ID, CLIENT_NAME, serverOptions, clientOptions);
        client.registerFeature(new CxxModulesFeature());
        client.onNotification('cxxModules/status', (params: CxxModulesStatus) => {
            this.commandLineTools.onStatus(params);
            this.status.update(withInstallCommandFallback(params));
        });
        // Messages from the server go to the log, never to notifications.
        client.onNotification(ShowMessageNotification.type, (params) => {
            this.log(`server ${messageTypeName(params.type)}: ${params.message}`);
        });
        client.onRequest(ShowMessageRequest.type, (params) => {
            this.log(`server ${messageTypeName(params.type)}: ${params.message}`);
            return null;
        });

        this.client = client;
        this.status.showStarting();
        this.log(`Starting ${launch.executable} ${args.join(' ')}`);
        try {
            await client.start();
            if (!this.status.lastStatus()) {
                this.status.showRunning();
            }
        } catch (error) {
            const reason = `The language server could not be started: ${errorText(error)}`;
            this.log(reason);
            this.status.showFailure(reason, true);
        }
    }

    private async stopNow(): Promise<void> {
        const client = this.client;
        this.client = undefined;
        if (!client) {
            return;
        }
        try {
            await client.dispose(5000);
        } catch (error) {
            this.log(`Stopping the language server: ${errorText(error)}`);
        }
    }

    private onClosed(): CloseHandlerResult {
        const now = Date.now();
        this.restarts = this.restarts.filter((time) => now - time < RESTART_WINDOW_MS);
        this.restarts.push(now);
        if (this.restarts.length <= MAX_RESTARTS) {
            this.log('The language server stopped unexpectedly and is being restarted.');
            this.status.showStarting('Restarting');
            return { action: CloseAction.Restart, handled: true };
        }
        const reason = 'The language server stopped repeatedly and was not restarted.';
        this.log(reason);
        this.status.showFailure(reason, true);
        return { action: CloseAction.DoNotRestart, handled: true };
    }
}

// In extension tests, count every call the extension (or the language client
// it bundles) makes to the small set of VS Code APIs that would otherwise put
// something in front of the user. Wrapping the shared vscode.window /
// vscode.languages objects before anything else runs -- the same approach the
// original notification-only counter used -- means every later call, from
// this extension's own code or from vscode-languageclient's internals, is
// observed regardless of which object created it.
//
// showTextDocument is different from the rest: the end-to-end suite itself
// opens the fixture's main.cpp through this same (necessarily shared)
// vscode.window object, so a live count would include the suite's own calls
// as well as the extension's. The extension's only path to showTextDocument
// is the opt-in "Show Module Graph" command, which none of the automated
// suites invoke, so the count the extension is responsible for is always
// whatever it is at the moment activate() is about to return -- before the
// suite has had any chance to call anything through the wrapped API. That
// value is captured once, in activate(), and handed back as a fixed number
// rather than read live. A suite that starts exercising Show Module Graph
// will need a different mechanism (snapshot immediately before running that
// command, and diff against a live reading afterwards).
function installUiCounters() {
    function countCalls(target: Record<string, unknown>, name: string, thisArg: unknown): () => number {
        const original = target[name];
        if (typeof original !== 'function') {
            return () => -1;
        }
        let count = 0;
        let installed = true;
        const wrapped = (...args: unknown[]): unknown => {
            count += 1;
            return (original as (...inner: unknown[]) => unknown).apply(thisArg, args);
        };
        try {
            target[name] = wrapped;
            installed = target[name] === wrapped;
        } catch {
            installed = false;
        }
        return () => (installed ? count : -1);
    }

    const window = vscode.window as unknown as Record<string, unknown>;
    const languages = vscode.languages as unknown as Record<string, unknown>;

    // Kept as one combined counter, as before the other counters existed:
    // the three show*Message methods are one concept (an unsolicited
    // notification) to everything that reads this counter.
    let notificationCount = 0;
    let notificationInstalled = true;
    for (const name of ['showInformationMessage', 'showWarningMessage', 'showErrorMessage']) {
        const original = window[name];
        if (typeof original !== 'function') {
            notificationInstalled = false;
            continue;
        }
        const wrapped = (...args: unknown[]): unknown => {
            notificationCount += 1;
            return (original as (...inner: unknown[]) => unknown).apply(vscode.window, args);
        };
        try {
            window[name] = wrapped;
            notificationInstalled = notificationInstalled && window[name] === wrapped;
        } catch {
            notificationInstalled = false;
        }
    }

    // createOutputChannel needs its own wrapper: what must be counted is
    // .show() calls on the channels this factory hands out ("channels the
    // extension created"), not calls to the factory itself.
    let outputChannelShowCount = 0;
    let outputChannelShowInstalled = true;
    const originalCreateOutputChannel = window['createOutputChannel'];
    if (typeof originalCreateOutputChannel !== 'function') {
        outputChannelShowInstalled = false;
    } else {
        const wrappedCreate = (...args: unknown[]): unknown => {
            const channel = (originalCreateOutputChannel as (...inner: unknown[]) => vscode.OutputChannel).apply(vscode.window, args);
            const channelRecord = channel as unknown as Record<string, unknown>;
            const originalShow = channelRecord['show'];
            if (typeof originalShow === 'function') {
                channelRecord['show'] = (...showArgs: unknown[]): unknown => {
                    outputChannelShowCount += 1;
                    return (originalShow as (...inner: unknown[]) => unknown).apply(channel, showArgs);
                };
            } else {
                outputChannelShowInstalled = false;
            }
            return channel;
        };
        try {
            window['createOutputChannel'] = wrappedCreate;
            outputChannelShowInstalled = window['createOutputChannel'] === wrappedCreate;
        } catch {
            outputChannelShowInstalled = false;
        }
    }

    return {
        notificationCount: () => (notificationInstalled ? notificationCount : -1),
        statusBarItemCount: countCalls(window, 'createStatusBarItem', vscode.window),
        outputChannelShowCount: () => (outputChannelShowInstalled ? outputChannelShowCount : -1),
        webviewPanelCount: countCalls(window, 'createWebviewPanel', vscode.window),
        showTextDocumentCount: countCalls(window, 'showTextDocument', vscode.window),
        languageStatusItemCount: countCalls(languages, 'createLanguageStatusItem', vscode.languages),
    };
}

let activeHost: ServerHost | undefined;

export function activate(context: vscode.ExtensionContext): TestApi {
    const ui = process.env.MCPPLS_TEST === '1' ? installUiCounters() : undefined;

    const status = new StatusController();
    // Forward-declared so the callbacks below can close over the eventual
    // ServerHost instance without restructuring construction order.
    let host: ServerHost;
    const commandLineTools = new CommandLineToolsController(context, (line) => host.log(line));

    let latestConflictCheck: Promise<ConflictCheck> = Promise.resolve('none-found');
    // Conflicts can appear or disappear after activation (another extension
    // installed, enabled, or its settings changed), so this re-runs every
    // time the server (re)starts, not only once at activation; the
    // already-answered guard in checkConflicts keeps that from asking twice.
    const runConflictCheck = (): void => {
        latestConflictCheck = checkConflicts(context, (line) => host.log(line)).catch((error): ConflictCheck => {
            host.log(`Checking for conflicting extensions: ${errorText(error)}`);
            return 'none-found';
        });
    };

    host = new ServerHost(context, status, commandLineTools, runConflictCheck);
    activeHost = host;
    context.subscriptions.push(status, host);

    const serverAccess = {
        runningClient: () => host.runningClient(),
        restart: () => host.restart(),
        showLogs: () => host.output().show(true),
        log: (line: string) => host.log(line),
    };
    registerCommands(context, serverAccess);

    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration('mcppls.compiler') || event.affectsConfiguration('mcppls.semanticKit')
                || event.affectsConfiguration('mcppls.engine') || event.affectsConfiguration('mcppls.buildTool')
                || event.affectsConfiguration('mcppls.toolEnvironment')) {
                void host.restart();
            }
        }),
        // Build description design 4.4: the user was told the build description needs a download,
        // went away to run the build tool, and came back. The server decides whether to act (it
        // acts at most once every thirty seconds) and reads the description again offline.
        vscode.window.onDidChangeWindowState((state) => {
            if (state.focused) void reloadBuildDescription(serverAccess);
        }),
        vscode.workspace.onDidGrantWorkspaceTrust(() => {
            void host.restart();
        }),
    );

    void host.start();

    // See installUiCounters: frozen now, before the test can have called
    // anything through the wrapped vscode.window object.
    const showTextDocumentAtActivation = ui?.showTextDocumentCount() ?? 0;

    return {
        waitForState: (state, timeoutMs) => status.waitForState(state, timeoutMs),
        lastStatus: () => status.lastStatus(),
        notificationCount: () => ui?.notificationCount() ?? 0,
        statusBarItemCount: () => ui?.statusBarItemCount() ?? 0,
        outputChannelShowCount: () => ui?.outputChannelShowCount() ?? 0,
        webviewPanelCount: () => ui?.webviewPanelCount() ?? 0,
        showTextDocumentCount: () => showTextDocumentAtActivation,
        languageStatusItemCount: () => ui?.languageStatusItemCount() ?? 0,
        conflictCheck: () => latestConflictCheck,
        setPromptAnswer: (kind, answer) => promptTestHarness?.setAnswer(kind, answer),
        promptShownCount: (kind) => promptTestHarness?.shownCount(kind) ?? 0,
        commandLineToolsInstallCount: () => commandLineTools.installInvocationCount(),
    };
}

export async function deactivate(): Promise<void> {
    const host = activeHost;
    activeHost = undefined;
    await host?.stop();
}
