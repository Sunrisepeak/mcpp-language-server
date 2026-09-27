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

import * as vscode from 'vscode';
import { askOnce } from './prompt';
import type { CxxModulesStatus } from './status';
import { needsDownload, shouldAsk } from './downloadAsk';

export const DESCRIBE_ONLINE_COMMAND = 'mcppls.describeOnline';
export const RUN_IN_TERMINAL_COMMAND = 'mcppls.runBuildToolInTerminal';
export const NEVER_KEY = 'mcppls.downloadPrompt.never';
export const ASKED_KEY = 'mcppls.downloadPrompt.asked';
export const FETCH = 'Download and Continue';
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
        const issue = needsDownload(status);
        if (issue) {
            this.pending.set(root, issue.message);
        } else {
            this.pending.delete(root);
        }
        const never = this.context.workspaceState.get<boolean>(NEVER_KEY, false);
        const askedAbout = this.context.workspaceState.get<string[]>(ASKED_KEY, []);
        if (!shouldAsk(issue, never, askedAbout, this.open.has(root)) || !issue) {
            return;
        }
        // Recorded before the answer: another status while the question is open must not ask again.
        this.open.add(root);
        void this.context.workspaceState.update(ASKED_KEY, [...askedAbout, issue.message].slice(-8));
        const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.parse(root))?.name ?? root;
        this.log(`asking whether to fetch what the build description of ${folder} needs`);
        void askOnce(
            'download',
            `C++ Modules: the build description of ${folder} needs a download. The project is served from its sources ` +
                'meanwhile. Fetch it now (the build tool may reach the network), or run the build yourself?',
            FETCH,
            RUN_IN_TERMINAL,
            NEVER,
        ).then((answer) => this.answered(root, answer));
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
        if (!this.pending.has(root)) {
            // §9.2 rule 4: the environment was completed meanwhile (a build in a terminal, the server's own retry).
            this.log('the build description no longer needs a download; nothing to do');
            return;
        }
        if (answer === FETCH) {
            this.log('fetching what the build description needs');
            void vscode.commands.executeCommand(DESCRIBE_ONLINE_COMMAND).then(undefined, (error: unknown) => {
                this.log(`could not ask the server to fetch it: ${error instanceof Error ? error.message : String(error)}`);
            });
        } else if (answer === RUN_IN_TERMINAL) {
            void vscode.commands.executeCommand(RUN_IN_TERMINAL_COMMAND);
        }
    }
}
