// The unrecoverable-error experience (0.0.7 plan): a status issue the server cannot recover from,
// or a server process that keeps dying, becomes one non-modal notification with what a person can
// do next, ending in a bug report that is already filled in.
//
// Never modal, and never awaited on the startup path: `show` returns at once and the answer is
// handled when it comes.

import * as os from 'os';
import * as vscode from 'vscode';
import { RESET_CACHE_COMMAND } from './cacheReset';
import { extensionEnvironment } from './commands';
import { writeCrashReport } from './crashReport';
import { buildIssueUrl } from './issueUrl';
import { notifyOnce } from './prompt';
import { redactText, Who } from './redact';
import {
    ACTION_LABELS, CRASH_ACTIONS, CRASH_DETAIL, crashMessage, FatalAction, FatalNotice, ModuleIssueLike, NoticeLedger, noticeText,
} from './unrecoverable';
import { TURN_OFF_COMMAND } from './enable';

export interface FatalHost {
    log(line: string): void;
    restart(): Promise<void>;
    showLogs(): void;
    serverVersion(): string | undefined;
    recentLog(): string[];
    serverStderr(): string[];
    serverLogFile(): string | undefined;
    // Exports a diagnostic bundle without asking; the written path, or undefined when none was written.
    exportBundle(): Promise<string | undefined>;
}

function whoAmI(): Who {
    let user = '';
    try {
        user = os.userInfo().username;
    } catch {
        // No user database entry: the home directory still goes.
    }
    return { home: os.homedir(), user };
}

export class FatalController {
    private readonly ledger = new NoticeLedger();
    // The newest crash report folder (the test API reads it).
    lastCrashReport: string | undefined;

    constructor(private readonly context: vscode.ExtensionContext, private readonly host: FatalHost) {}

    // Every status the server sends. Returns the notices shown, for tests.
    onIssues(issues: readonly ModuleIssueLike[] | undefined): FatalNotice[] {
        const notices = this.ledger.take(issues);
        for (const notice of notices) {
            this.host.log(`unrecoverable: [${notice.code}] ${notice.message}${notice.bundle ? ` (bundle: ${notice.bundle})` : ''}`);
            const labels = notice.actions.map((action) => ACTION_LABELS[action]);
            void notifyOnce('unrecoverable', 'error', noticeText(notice), labels).then(
                (choice) => this.run(choice, notice.actions, () => this.reportForIssue(notice)),
                () => undefined);
        }
        return notices;
    }

    // The client's restart budget is used up: write the crash report, tell the person.
    onCrashLoop(crashes: number): void {
        let directory: string | undefined;
        try {
            directory = writeCrashReport({
                storageDirectory: this.context.globalStorageUri.fsPath,
                now: new Date(),
                crashes,
                clientLog: this.host.recentLog(),
                serverStderr: this.host.serverStderr(),
                serverLogFile: this.host.serverLogFile(),
                versions: { ...extensionEnvironment(), server: this.host.serverVersion() ?? null },
                who: whoAmI(),
            }).directory;
            this.lastCrashReport = directory;
            this.host.log(`crash report written: ${directory}`);
        } catch (error) {
            this.host.log(`the crash report could not be written: ${error instanceof Error ? error.message : String(error)}`);
        }
        const message = crashMessage(crashes);
        const text = `C++ Modules: ${message}. ${directory ? CRASH_DETAIL : 'The crash report could not be saved; Show Logs has the details.'}`;
        void notifyOnce('unrecoverable', 'error', text, CRASH_ACTIONS.map((action) => ACTION_LABELS[action])).then(
            (choice) => this.run(choice, CRASH_ACTIONS, async () => {
                await this.openReport({ code: 'server-crashed', message, path: directory, kind: 'crash-report' });
            }),
            () => undefined);
    }

    private async run(choice: string | undefined, actions: readonly FatalAction[], report: () => Promise<void>): Promise<void> {
        const action = actions.find((candidate) => ACTION_LABELS[candidate] === choice);
        switch (action) {
            case 'reportIssue':
                await report();
                break;
            case 'restartServer':
                await this.host.restart();
                break;
            case 'resetCache':
                await vscode.commands.executeCommand(RESET_CACHE_COMMAND);
                break;
            case 'turnOff':
                await vscode.commands.executeCommand(TURN_OFF_COMMAND);
                break;
            case 'showLogs':
                this.host.showLogs();
                break;
            case undefined:
                break;
        }
    }

    private async reportForIssue(notice: FatalNotice): Promise<void> {
        let bundle = notice.bundle;
        if (!bundle) {
            // An older server wrote none: write one now, so the person has something to attach.
            try {
                bundle = await this.host.exportBundle();
            } catch (error) {
                this.host.log(`the diagnostic bundle could not be written: ${error instanceof Error ? error.message : String(error)}`);
            }
        }
        await this.openReport({ code: notice.code, message: notice.message, path: bundle, kind: 'bundle' });
    }

    // Shows the file where it is (so it can be attached) and opens the form.
    private async openReport(what: { code: string; message: string; path: string | undefined; kind: 'bundle' | 'crash-report' }): Promise<void> {
        if (what.path) {
            try {
                await vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(what.path));
            } catch (error) {
                this.host.log(`could not reveal ${what.path}: ${error instanceof Error ? error.message : String(error)}`);
            }
        }
        const url = buildIssueUrl({
            code: what.code,
            message: what.message,
            extensionVersion: (extensionEnvironment().version as string | undefined),
            serverVersion: this.host.serverVersion(),
            appName: vscode.env.appName,
            editorVersion: vscode.version,
            platform: process.platform,
            arch: process.arch,
            bundlePath: what.path ? redactText(what.path, whoAmI()) : undefined,
            bundleKind: what.kind,
        });
        this.host.log(`opening the issue form: ${url}`);
        if (process.env.MCPPLS_TEST === '1') return;
        await vscode.env.openExternal(vscode.Uri.parse(url));
    }
}
