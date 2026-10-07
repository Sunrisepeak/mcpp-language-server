// C++ Modules for VS Code: a thin client for the mcppls language server.
//
// The extension starts the server from the bundled payload, advertises the
// cxxModules protocol extension, shows one language status item, and otherwise
// stays out of sight: no walkthroughs, and a notification only for what the server cannot
// recover from (src/fatal.ts) or a server process that keeps dying.

import * as readline from 'readline';
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
    MessageTransports,
    RevealOutputChannelOn,
    ServerOptions,
    ShowMessageNotification,
    ShowMessageRequest,
    State,
    StaticFeature,
    TransportKind,
} from 'vscode-languageclient/node';
import { TransportWriter } from './transportWriter';
import { CommandLineToolsController, withInstallCommandFallback } from './commandLineTools';
import { DownloadPromptController } from './downloadPrompt';
import { declaresModules, editorEnvironment, exportDiagnosticBundle, extensionEnvironment, registerCommands, reloadBuildDescription } from './commands';
import { fetchCacheReport } from './cacheHubView';
import { sendTriggeredCompletion } from './completionGate';
import { checkConflicts, ConflictCheck, watchForNewConflicts } from './conflicts';
import { CrashCounter } from './crashCounter';
import { serverEnabled } from './enable';
import { transition } from './enableSwitch';
import { FatalController } from './fatal';
import { resolveLaunch } from './payload';
import { ServerLogLevel, ServerLogRouter } from './serverLog';
import { promptTestHarness, PromptKind, ShownPrompt } from './prompt';
import { CxxModulesStatus, ModuleIssue, ModuleState, StatusController } from './status';
import { describeActiveWorkarounds } from './workarounds';
import { overriddenByLanguageDefault } from './quickSuggestions';
import { buildInitializationOptions } from './settingsRead';
import { offerSettingsMigration } from './settingsMigration';
import { setLocalizer } from './strings';

const CLIENT_ID = 'mcppls';
const CLIENT_NAME = 'C++ Modules';
const RECENT_LOG_LINES = 1000;
const STDERR_TAIL_LINES = 500;

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
    // Server stderr lines written to the log at each level (src/serverLog.ts).
    serverLogLineCount(level: ServerLogLevel): number;
    // The extension's part of a report and of the diagnostic bundle: versions, the editor's appName, host, UI kind.
    environment(): Record<string, unknown>;
    // The editor settings of that part that decide whether completion shows while typing (0.0.8 plan E-3).
    editor(): Record<string, unknown>;
    // The commands the running server lists in `executeCommandProvider` (empty when it is not running).
    serverCommands(): string[];
    // One `cxxModules/cache` answer from the running server, the report the hub draws (S3 5.7).
    cacheDetail(): Promise<import('./cacheSegment').CacheDetail | undefined>;
    // What the last unrecoverable-error notification offered (test mode; nothing is put on screen).
    lastPrompt(kind: PromptKind): ShownPrompt | undefined;
    // Hands the notification logic the issues of a status, as if the server had sent them; returns the
    // codes it showed a notification for. Test mode only.
    injectIssues(issues: { code: string; message: string; category?: string; bundle?: string }[]): string[];
    // Whether the server is running, and whether mcppls.enable lets it.
    serverRunning(): boolean;
    serverEnabled(): boolean;
    statusBarText(): string;
    // The folder of the newest crash report written this session.
    lastCrashReport(): string | undefined;
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

function lines(input: NodeJS.ReadableStream, onLine: (line: string) => void): void {
    readline.createInterface({ input, crlfDelay: Infinity, terminal: false, historySize: 0 }).on('line', onLine);
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

// WA-VSCODE-003: initialization failure calls stop() without awaiting its promise in vscode-languageclient.
// Once the transport has closed, there is no server left to shut down; a failed shutdown must
// not become an unhandled rejection in the extension host. Other stop failures still propagate.
class ServerLanguageClient extends LanguageClient {
    private transportClosed = false;

    protected override async createMessageTransports(encoding: string): Promise<MessageTransports> {
        const transports = await super.createMessageTransports(encoding);
        return { ...transports, writer: new TransportWriter(transports.writer) };
    }

    protected override handleConnectionClosed(): Promise<void> {
        this.transportClosed = true;
        return super.handleConnectionClosed();
    }

    override async stop(timeout?: number): Promise<void> {
        try {
            await super.stop(timeout);
        } catch (error) {
            if (!this.transportClosed) throw error;
        }
    }
}

class ServerHost implements vscode.Disposable {
    private client: LanguageClient | undefined;
    private initializedClient: LanguageClient | undefined;
    private startup: Promise<void> | undefined;
    private channel: vscode.LogOutputChannel | undefined;
    readonly serverLog = new ServerLogRouter();
    private readonly crashes = new CrashCounter();
    private readonly stderrTail: string[] = [];
    private knownServerLog: string | undefined;
    private knownServerVersion: string | undefined;
    private queue: Promise<void> = Promise.resolve();
    private readonly recent: string[] = [];

    constructor(
        private readonly context: vscode.ExtensionContext,
        private readonly status: StatusController,
        private readonly commandLineTools: CommandLineToolsController,
        // Called every time the server actually (re)starts, i.e. once at
        // activation and once per mcppls.restartServer or a config-driven
        // restart, but not for automatic crash recovery. Used to re-run
        // conflict detection at the same points a fresh cxxModules/status
        // naturally re-triggers the Command Line Tools check, so both
        // one-time questions are exercised the same way by a restart.
        private readonly onStarting: () => void,
        private readonly hooks: {
            // mcppls.enable: false keeps the server from starting at all.
            isEnabled: () => boolean;
            // The client's restart budget is used up.
            onCrashLoop: (crashes: number) => void;
            // The issues of every status the server sends.
            onIssues: (issues: CxxModulesStatus['issues']) => void;
        },
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
        const stamped = `[${new Date().toLocaleTimeString()}] ${line}`;
        this.output().appendLine(stamped);
        // A diagnostic bundle carries the extension's own log (issue #23 fix plan F18); the output
        // channel cannot be read back, so its latest lines are kept here too.
        this.recent.push(stamped);
        if (this.recent.length > RECENT_LOG_LINES) {
            this.recent.splice(0, this.recent.length - RECENT_LOG_LINES);
        }
    }

    recentLog(): string[] {
        return [...this.recent];
    }

    stderrLines(): string[] {
        return [...this.stderrTail];
    }

    serverLogFile(): string | undefined {
        return this.knownServerLog;
    }

    serverVersion(): string | undefined {
        return this.knownServerVersion;
    }

    runningClient(): LanguageClient | undefined {
        // State.Running is emitted before the library has registered the server's features.
        return this.client && this.client === this.initializedClient && this.client.state === State.Running ? this.client : undefined;
    }

    start(): Promise<void> {
        return this.enqueue(() => this.startNow());
    }

    stop(): Promise<void> {
        return this.enqueue(() => this.stopNow());
    }

    restart(): Promise<void> {
        return this.enqueue(async () => {
            this.crashes.reset();
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

    private async startNow(recovery = false): Promise<void> {
        if (this.client) {
            return;
        }
        if (!this.hooks.isEnabled()) {
            this.log('mcppls.enable is false for this workspace: the language server is not started.');
            this.status.showOff();
            return;
        }
        if (!recovery) this.onStarting();
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

        // The client this options object belongs to, for the close handler (see onClosed).
        let created: LanguageClient | undefined;
        const compiler = (configuration.get<string>('compiler') ?? '').trim();
        const clientOptions: LanguageClientOptions = {
            documentSelector: [
                { scheme: 'file', language: 'cpp' },
                { scheme: 'file', language: 'c' },
            ],
            outputChannel: this.output(),
            revealOutputChannelOn: RevealOutputChannelOn.Never,
            // Over stdio, stdout is the protocol and never reaches `stdout` below; the option takes
            // both, so it keeps the client's default. stderr is the server's log, written at the
            // level each line carries rather than all as errors.
            stdioOptions: {
                stdout: (input, channel) => {
                    lines(input, (line) => channel.info(line));
                },
                stderr: (input, channel) => {
                    this.serverLog.reset();
                    lines(input, (line) => {
                        this.serverLog.route(line, channel);
                        this.stderrTail.push(line);
                        if (this.stderrTail.length > STDERR_TAIL_LINES) {
                            this.stderrTail.splice(0, this.stderrTail.length - STDERR_TAIL_LINES);
                        }
                    });
                },
            },
            initializationOptions: buildInitializationOptions(configuration, compiler),
            middleware: {
                // Fix plan 2026-09-26 F9 (D4 layer 1): of the completions a typed space asks for, only
                // the one after `import` or `export import` is sent; every other is answered here, with
                // nothing, before it costs a message.
                provideCompletionItem: (document, position, context, token, next) =>
                    sendTriggeredCompletion(context.triggerCharacter, document.lineAt(position.line).text, position.character)
                        ? next(document, position, context, token)
                        : [],
            },
            errorHandler: {
                error: () => ({ action: ErrorAction.Continue, handled: true }),
                closed: () => this.onClosed(created),
            },
            initializationFailedHandler: (error) => {
                const reason = `The language server failed to initialize: ${errorText(error)}`;
                this.log(reason);
                this.status.showFailure(reason, true);
                return false;
            },
        };

        const client = new ServerLanguageClient(CLIENT_ID, CLIENT_NAME, serverOptions, clientOptions);
        created = client;
        client.registerFeature(new CxxModulesFeature());
        client.onNotification('cxxModules/status', (params: CxxModulesStatus) => {
            this.commandLineTools.onStatus(params);
            this.status.update(withInstallCommandFallback(params));
            this.hooks.onIssues(params.issues);
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
            this.startup = client.start();
            await this.startup;
            if (this.client !== client) return;
            this.initializedClient = client;
            if (!this.status.lastStatus()) {
                this.status.showRunning();
            }
            this.knownServerVersion = client.initializeResult?.serverInfo?.version;
            void this.learnServerLog(client);
        } catch (error) {
            if (this.client !== client) return;
            const reason = `The language server could not be started: ${errorText(error)}`;
            this.log(reason);
            this.status.showFailure(reason, true);
        }
    }

    private async stopNow(): Promise<void> {
        const client = this.client;
        this.client = undefined;
        this.initializedClient = undefined;
        if (!client) {
            return;
        }
        // The library refuses to stop a client in Starting: it
        // throws before killing the process it has just spawned, which would then outlive this
        // host. Let the start settle first; dispose then stops whatever it started.
        if (client.state === State.Starting) {
            try {
                await client.start();
            } catch {
                // A start that failed has nothing running; dispose below is still safe.
            }
        }
        try {
            await client.dispose(5000);
        } catch (error) {
            this.log(`Stopping the language server: ${errorText(error)}`);
        }
    }

    // What the server's report says about where its log is, so a crash report can carry the tail of
    // it after the process is gone. Best effort, once per start; a server that says nothing leaves
    // the crash report with the stderr the client saw.
    private async learnServerLog(client: LanguageClient): Promise<void> {
        if (!declaresModules(client.initializeResult?.capabilities)) return;
        try {
            const report = await Promise.race([
                client.sendRequest<{ server?: { logFile?: string; version?: string } }>('cxxModules/report', {}),
                new Promise<undefined>((resolve) => setTimeout(() => resolve(undefined), 30000)),
            ]);
            if (typeof report?.server?.logFile === 'string' && report.server.logFile.length > 0) {
                this.knownServerLog = report.server.logFile;
            }
            if (typeof report?.server?.version === 'string' && !this.knownServerVersion) {
                this.knownServerVersion = report.server.version;
            }
        } catch {
            // The request can fail while the server is stopping; nothing depends on it.
        }
    }

    // The client's close handler: the server process ended. A close the extension asked for (stop,
    // restart, turning the server off, dispose) is never counted: by then `this.client` is no longer
    // this client, and the library does not call the handler for a stop it made itself.
    private async onClosed(closed: LanguageClient | undefined): Promise<CloseHandlerResult> {
        // The connection has already been disposed by the library. Let initialize's continuation
        // settle before handleConnectionClosed clears its features and resets its start promise.
        // Otherwise the reset can orphan a rejected start promise, or initialization can register
        // commands after cleanup has passed them.
        if (closed === this.client) {
            try {
                await this.startup;
            } catch {
                // The transport closing during initialize is itself the crash being counted.
            }
        }
        const verdict = this.crashes.record(Date.now(), closed === undefined || closed !== this.client);
        if (verdict.count === 0) {
            return { action: CloseAction.DoNotRestart, handled: true };
        }
        this.initializedClient = undefined;
        if (!verdict.giveUp) {
            this.log('The language server stopped unexpectedly and is being restarted.');
            this.status.showStarting('Restarting');
            // Let handleConnectionClosed finish clearing the old client's features before the
            // host creates its replacement. Reusing the library's automatic restart can overlap
            // two initialize continuations when status arrives before feature registration ends,
            // leaving commands such as clangd.applyFix registered twice. Recovery shares the
            // manual lifecycle queue and preserves the crash budget.
            setImmediate(() => {
                void this.enqueue(async () => {
                    if (this.client !== closed) return;
                    await this.stopNow();
                    await this.startNow(true);
                }).catch((error) => this.log(`Restarting the language server: ${errorText(error)}`));
            });
            return { action: CloseAction.DoNotRestart, handled: true, message: 'Connection to server closed; the extension is scheduling recovery.' };
        }
        const reason = `The language server stopped after crashing ${verdict.count} times and was not restarted.`;
        this.log(reason);
        this.status.showFailure(reason, true);
        this.hooks.onCrashLoop(verdict.count);
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
    // UI-1 (plan 2026-10-03): every user-facing word goes through strings.ts, and the editor's own
    // localization picks the bundle (l10n/bundle.l10n.*.json) by the display language -- English is
    // the source, zh-cn the one translation. First thing activation does: nothing renders before it.
    setLocalizer((message, ...args) => vscode.l10n.t(message, ...args));

    const ui = process.env.MCPPLS_TEST === '1' ? installUiCounters() : undefined;

    const status = new StatusController();
    // Forward-declared so the callbacks below can close over the eventual
    // ServerHost instance without restructuring construction order.
    let host: ServerHost;
    const commandLineTools = new CommandLineToolsController(context, (line) => host.log(line));
    const downloadPrompt = new DownloadPromptController(context, (line) => host.log(line));
    status.onUpdate((current) => downloadPrompt.onStatus(current));
    context.subscriptions.push(vscode.commands.registerCommand('mcppls.askBeforeDownloading', () => downloadPrompt.askBeforeDownloading()));

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

    // Forward-declared like `host`: the controller needs the host's log and restart, the host needs
    // the controller for what it tells a person when the server will not stay up.
    let fatal: FatalController;
    host = new ServerHost(context, status, commandLineTools, runConflictCheck, {
        isEnabled: serverEnabled,
        onCrashLoop: (crashes) => fatal.onCrashLoop(crashes),
        onIssues: (issues) => { fatal.onIssues(issues); },
    });
    fatal = new FatalController(context, {
        log: (line) => host.log(line),
        restart: () => host.restart(),
        showLogs: () => host.output().show(true),
        serverVersion: () => host.serverVersion(),
        recentLog: () => host.recentLog(),
        serverStderr: () => host.stderrLines(),
        serverLogFile: () => host.serverLogFile(),
        exportBundle: () => exportDiagnosticBundle(serverAccess, false, true),
    });
    activeHost = host;
    context.subscriptions.push(status, host);
    // Workaround registry design (§9): visible once per activation, so a bug report shows which of
    // this extension's own workarounds (as opposed to the server's) were on.
    host.log(describeActiveWorkarounds());
    // WA-VSCODE-002 (0.0.8 plan E-2): the language default for C and C++ outranks a setting that names no
    // language, so whoever set `editor.quickSuggestions` for every language is told, once, how to keep theirs.
    const quickSuggestionsOverridden = overriddenByLanguageDefault(
        vscode.workspace.getConfiguration('editor', { languageId: 'cpp' }).inspect('quickSuggestions'));
    if (quickSuggestionsOverridden) host.log(quickSuggestionsOverridden);
    // S-1 (plan 0.0.9): a setting of theirs under a renamed name keeps working; they are told once, and
    // nothing is written unless they click.
    void offerSettingsMigration(context, (line) => host.log(line));
    // Coexistence design (§10): a conflict that becomes active after activation -- another C++
    // extension installed, enabled, or its setting turned back on -- gets a notice, once per
    // conflict per session, distinct from the one-time question above.
    context.subscriptions.push(watchForNewConflicts(context, (line) => host.log(line)));

    const serverAccess = {
        runningClient: () => host.runningClient(),
        restart: () => host.restart(),
        showLogs: () => host.output().show(true),
        log: (line: string) => host.log(line),
        recentLog: () => host.recentLog(),
    };
    registerCommands(context, serverAccess);
    // The hover card's table needs the `cxxModules/cache` detail; this makes it arrive without the
    // hub being opened first (2026-10-03 UI-2). `host` is the forward-declared instance by now.
    status.setCacheDetailFetcher(() => fetchCacheReport(host.runningClient()));

    // The per-workspace off switch: while it is off nothing is started, and turning it on or off at
    // runtime starts or stops the server.
    let wasEnabled = serverEnabled();
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration('mcppls.enable')) {
                const nowEnabled = serverEnabled();
                const change = transition(wasEnabled, nowEnabled);
                wasEnabled = nowEnabled;
                if (change === 'start') {
                    host.log('mcppls.enable is on again: starting the language server.');
                    void host.start();
                } else if (change === 'stop') {
                    host.log('mcppls.enable is false for this workspace: stopping the language server.');
                    void host.stop().then(() => status.showOff());
                }
            }
            if (event.affectsConfiguration('mcppls.compiler') || event.affectsConfiguration('mcppls.semanticKit')
                // S-1, S-2 (plan 0.0.9): `mcppls.engine` and `mcppls.buildDiscovery` still match, as the old
                // names a hand edit may touch; the new ones are named so the list says what it restarts for.
                || event.affectsConfiguration('mcppls.engine') || event.affectsConfiguration('mcppls.engine.name')
                || event.affectsConfiguration('mcppls.format.fallbackStyle') || event.affectsConfiguration('mcppls.engine.workers') || event.affectsConfiguration('mcppls.buildTool')
                || event.affectsConfiguration('mcppls.toolEnvironment') || event.affectsConfiguration('mcppls.semanticTokens.modules')
                || event.affectsConfiguration('mcppls.completion.triggerOnSpace')
                // 0.0.6 plan §3.7 B-7, §2.6/§9 T5: new settings, same treatment as the ones above.
                || event.affectsConfiguration('mcppls.buildDiscovery') || event.affectsConfiguration('mcppls.buildDiscovery.mode')
                || event.affectsConfiguration('mcppls.buildDiscovery.providers')
                || event.affectsConfiguration('mcppls.buildDiscovery.askBeforeDownload')
                || event.affectsConfiguration('mcppls.index.primeImplementationUnits')) {
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
        serverLogLineCount: (level) => host.serverLog.count(level),
        environment: () => extensionEnvironment(),
        editor: () => editorEnvironment(),
        serverCommands: () => [...(host.runningClient()?.initializeResult?.capabilities.executeCommandProvider?.commands ?? [])],
        // The hub's own view of the cache, through the same `cxxModules/cache` round trip it makes
        // (S3 5.7): the end-to-end tests assert the envelope's fields on the real server.
        cacheDetail: async () => fetchCacheReport(host.runningClient()),
        lastPrompt: (kind) => promptTestHarness?.lastShown(kind),
        injectIssues: (issues) => promptTestHarness ? fatal.onIssues(issues as ModuleIssue[]).map((notice) => notice.code) : [],
        serverRunning: () => host.runningClient() !== undefined,
        serverEnabled: () => serverEnabled(),
        statusBarText: () => status.barText(),
        lastCrashReport: () => fatal.lastCrashReport,
    };
}

export async function deactivate(): Promise<void> {
    const host = activeHost;
    activeHost = undefined;
    await host?.stop();
}
