// The build description needs a download (plan 2026-09-27 B-2, §9.2). The server runs the build tool
// offline, and when it cannot describe the project without fetching something it says so in the status,
// with `askOnline` when a client may offer to fetch it. This is the third question the extension ever
// asks (conflicts.ts and commandLineTools.ts are the others), and it follows the rules of §9.2:
//
//   - it never blocks anything: a notification in the corner, which may stay unanswered forever; nothing
//     the extension or the server does waits for it, and the project is served from its sources meanwhile;
//   - it is asked once per workspace for one set of missing things, and never again after "Don't Ask Again";
//   - the answer can come long after the question. By then the person may have built the project in their
//     own terminal, or the server's own retries may have found everything in place; an answer to a question
//     that no longer applies does nothing.
//
// D-4 (plan 0.0.9): "Always Download in This Workspace" makes the answer the default for this workspace (kept in
// workspaceState, never in the project's files): what the server offers to fetch is fetched without asking, and
// "C++ Modules: Ask Before Downloading in This Workspace" takes it back. The build tool still runs offline every
// other time. D-5: how a fetch the person asked for ended is told once, from the status's `onlineRun`.

import * as vscode from 'vscode';
import { askOnce, notifyOnce } from './prompt';
import type { CxxModulesStatus } from './status';
import { downloadAction, needsDownload, onlineRunToTell } from './downloadAsk';

export const DESCRIBE_ONLINE_COMMAND = 'mcppls.describeOnline';
export const RUN_IN_TERMINAL_COMMAND = 'mcppls.runBuildToolInTerminal';
export const SHOW_LOGS_COMMAND = 'mcppls.showLogs';
export const NEVER_KEY = 'mcppls.downloadPrompt.never';
export const ASKED_KEY = 'mcppls.downloadPrompt.asked';
export const ALWAYS_KEY = 'mcppls.downloadPrompt.always';
export const TOLD_KEY = 'mcppls.downloadPrompt.told';
export const FETCH = 'Download and Continue';
export const ALWAYS = 'Always Download in This Workspace';
export const SHOW_LOGS = 'Show Logs';
export const RUN_IN_TERMINAL = 'Run in Terminal';
export const NEVER = "Don't Ask Again";

export class DownloadPromptController {
    // What each root still needs, from its latest status: the answer to an old question is checked against it.
    private readonly pending = new Map<string, string>();
    private readonly open = new Set<string>();

    constructor(private readonly context: vscode.ExtensionContext, private readonly log: (line: string) => void) {}

    // Called for every cxxModules/status notification from the running server.
    onStatus(status: CxxModulesStatus): void {
        const root = status.project.root;
        this.tellOnlineRun(status);
        const issue = needsDownload(status);
        if (issue) {
            this.pending.set(root, issue.message);
        } else {
            this.pending.delete(root);
        }
        const never = this.context.workspaceState.get<boolean>(NEVER_KEY, false);
        const allowed = this.context.workspaceState.get<boolean>(ALWAYS_KEY, false);
        const askedAbout = this.context.workspaceState.get<string[]>(ASKED_KEY, []);
        const action = downloadAction(issue, never, allowed, askedAbout, this.open.has(root));
        if (action === 'none' || !issue) {
            return;
        }
        // Recorded before the answer: another status while the question is open must not ask again.
        void this.context.workspaceState.update(ASKED_KEY, [...askedAbout, issue.message].slice(-8));
        const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.parse(root))?.name ?? root;
        if (action === 'fetch') {
            this.log(`fetching what the build description of ${folder} needs: downloads are allowed in this workspace`);
            this.fetch();
            return;
        }
        this.open.add(root);
        this.log(`asking whether to fetch what the build description of ${folder} needs`);
        void askOnce(
            'download',
            `C++ Modules: the build description of ${folder} needs a download. The project is served from its sources ` +
                'meanwhile. Fetch it now (the build tool may reach the network), or run the build yourself? ' +
                'The status bar item says what is missing.',
            FETCH,
            ALWAYS,
            RUN_IN_TERMINAL,
            NEVER,
        ).then((answer) => this.answered(root, answer));
    }

    // "C++ Modules: Ask Before Downloading in This Workspace": takes "Always Download" back, and "Don't Ask Again".
    askBeforeDownloading(): void {
        void this.context.workspaceState.update(ALWAYS_KEY, false);
        void this.context.workspaceState.update(NEVER_KEY, false);
        this.log('downloads for the build description are asked about again in this workspace');
        void vscode.window.showInformationMessage('C++ Modules: a download the build description needs will be asked about first in this workspace.');
    }

    private fetch(): void {
        void vscode.commands.executeCommand(DESCRIBE_ONLINE_COMMAND).then(undefined, (error: unknown) => {
            this.log(`could not ask the server to fetch it: ${error instanceof Error ? error.message : String(error)}`);
        });
    }

    // D-5: once per run, whether it fetched what was needed or why not.
    private tellOnlineRun(status: CxxModulesStatus): void {
        const told = this.context.workspaceState.get<string[]>(TOLD_KEY, []);
        const run = onlineRunToTell(status, told);
        if (!run) return;
        void this.context.workspaceState.update(TOLD_KEY, [...told, run.at].slice(-8));
        const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.parse(status.project.root))?.name ?? status.project.root;
        this.log(`the fetch for ${folder} ${run.outcome === 'fetched' ? 'succeeded' : 'failed'}: ${run.message}`);
        if (run.outcome === 'fetched') {
            void notifyOnce('downloadResult', 'info', `C++ Modules: ${folder}: ${run.message}`, []);
            return;
        }
        void notifyOnce('downloadResult', 'warning', `C++ Modules: ${folder}: ${run.message}`, [SHOW_LOGS, RUN_IN_TERMINAL]).then((choice) => {
            if (choice === SHOW_LOGS) void vscode.commands.executeCommand(SHOW_LOGS_COMMAND);
            else if (choice === RUN_IN_TERMINAL) void vscode.commands.executeCommand(RUN_IN_TERMINAL_COMMAND);
        });
    }

    private answered(root: string, answer: string | undefined): void {
        this.open.delete(root);
        if (answer === NEVER) {
            void this.context.workspaceState.update(NEVER_KEY, true);
            this.log('the build description download will not be offered again in this workspace');
            return;
        }
        if (answer === undefined) {
            return;
        }
        if (answer === ALWAYS) {
            void this.context.workspaceState.update(ALWAYS_KEY, true);
            this.log('downloads the build description needs are fetched without asking in this workspace');
        }
        if (!this.pending.has(root)) {
            // §9.2 rule 4: the environment was completed meanwhile (a build in a terminal, the server's own retry).
            this.log('the build description no longer needs a download; nothing to do');
            return;
        }
        if (answer === FETCH || answer === ALWAYS) {
            this.log('fetching what the build description needs');
            this.fetch();
        } else if (answer === RUN_IN_TERMINAL) {
            void vscode.commands.executeCommand(RUN_IN_TERMINAL_COMMAND);
        }
    }
}
