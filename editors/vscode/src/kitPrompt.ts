// The semantic kit of a target is not installed (ecosystem design 2026-10-29 §9.2): the server reads the
// project with the openkal kit meanwhile and says what is missing. This asks, the way downloadPrompt.ts asks
// about a build description's download (S3-4-19 to S3-4-21 hold here too):
//
//   - it never blocks anything: a notification in the corner, which may stay unanswered; the project is
//     served with the openkal kit meanwhile;
//   - it is asked once per workspace for one kit, and never again after "Don't Ask Again";
//   - "Always Install Kits" makes the answer the default for this workspace (workspaceState, never the
//     project's files); "C++ Modules: Ask Before Installing Kits in This Workspace" takes it back;
//   - an answer to a question that no longer applies (the kit was installed by hand meanwhile) does nothing.
//
// How an install ended is told once, from the status's `kitInstall`.

import * as vscode from 'vscode';
import { askOnce, notifyOnce } from './prompt';
import type { CxxModulesStatus } from './status';
import { kitAction, kitInstallToTell, kitKey, kitMissing } from './kitAsk';

export const INSTALL_KIT_COMMAND = 'mcppls.installKit';
export const SHOW_LOGS_COMMAND = 'mcppls.showLogs';
export const NEVER_KEY = 'mcppls.kitPrompt.never';
export const ASKED_KEY = 'mcppls.kitPrompt.asked';
export const ALWAYS_KEY = 'mcppls.kitPrompt.always';
export const TOLD_KEY = 'mcppls.kitPrompt.told';
export const INSTALL = 'Install';
export const ALWAYS = 'Always Install Kits';
export const COPY_COMMAND = 'Copy Command';
export const NEVER = "Don't Ask Again";
export const SHOW_LOGS = 'Show Logs';

export class KitPromptController {
    // The kit each root still lacks, from its latest status: the answer to an old question is checked against it.
    private readonly pending = new Map<string, string>();
    private readonly open = new Set<string>();

    constructor(private readonly context: vscode.ExtensionContext, private readonly log: (line: string) => void) {}

    // Called for every cxxModules/status notification from the running server.
    onStatus(status: CxxModulesStatus): void {
        const root = status.project.root;
        this.tellInstall(status);
        const issue = kitMissing(status);
        if (issue?.kit) {
            this.pending.set(root, issue.kit.target);
        } else {
            this.pending.delete(root);
        }
        const never = this.context.workspaceState.get<boolean>(NEVER_KEY, false);
        const always = this.context.workspaceState.get<boolean>(ALWAYS_KEY, false);
        const askedAbout = this.context.workspaceState.get<string[]>(ASKED_KEY, []);
        const action = kitAction(issue, never, always, askedAbout, this.open.has(root));
        if (action === 'none' || !issue) {
            return;
        }
        // Recorded before the answer: another status while the question is open must not ask again.
        void this.context.workspaceState.update(ASKED_KEY, [...askedAbout, kitKey(issue)].slice(-8));
        const target = issue.kit?.target ?? '';
        if (action === 'install') {
            this.log(`installing ${issue.kit?.package ?? 'the semantic kit'}: kits are installed without asking in this workspace`);
            this.install(target);
            return;
        }
        this.open.add(root);
        this.log(`asking whether to install ${issue.kit?.package ?? 'the semantic kit'}`);
        void askOnce(
            'kit',
            `C++ Modules: the semantic kit for ${target} is not installed, so files are read with the openkal kit: ` +
                'import std and the standard library work, this system\'s own headers and macros do not. ' +
                `Install ${issue.kit?.package ?? 'it'} in the background (xlings reaches the network)?`,
            INSTALL,
            ALWAYS,
            COPY_COMMAND,
            NEVER,
        ).then((answer) => this.answered(root, answer, issue.kit?.command));
    }

    // "C++ Modules: Ask Before Installing Kits in This Workspace": takes "Always Install Kits" back, and "Don't Ask Again".
    askBeforeInstalling(): void {
        void this.context.workspaceState.update(ALWAYS_KEY, false);
        void this.context.workspaceState.update(NEVER_KEY, false);
        void this.context.workspaceState.update(ASKED_KEY, []);
        this.log('installing a semantic kit is asked about again in this workspace');
        void vscode.window.showInformationMessage('C++ Modules: a semantic kit will be asked about before it is installed in this workspace.');
    }

    private install(target: string): void {
        void vscode.commands.executeCommand(INSTALL_KIT_COMMAND, { target }).then(undefined, (error: unknown) => {
            this.log(`could not ask the server to install the kit: ${error instanceof Error ? error.message : String(error)}`);
        });
    }

    // Once per install, whether it installed the kit or why not.
    private tellInstall(status: CxxModulesStatus): void {
        const told = this.context.workspaceState.get<string[]>(TOLD_KEY, []);
        const run = kitInstallToTell(status, told);
        if (!run) return;
        void this.context.workspaceState.update(TOLD_KEY, [...told, run.at].slice(-8));
        this.log(`the semantic kit install ${run.outcome === 'installed' ? 'succeeded' : 'failed'}: ${run.message}`);
        if (run.outcome === 'installed') {
            void notifyOnce('kitResult', 'info', `C++ Modules: ${run.message}; the project is read with it now.`, []);
            return;
        }
        void notifyOnce('kitResult', 'warning', `C++ Modules: ${run.message}`, [SHOW_LOGS]).then((choice) => {
            if (choice === SHOW_LOGS) void vscode.commands.executeCommand(SHOW_LOGS_COMMAND);
        });
    }

    private answered(root: string, answer: string | undefined, command: string | undefined): void {
        this.open.delete(root);
        if (answer === NEVER) {
            void this.context.workspaceState.update(NEVER_KEY, true);
            this.log('installing a semantic kit will not be offered again in this workspace');
            return;
        }
        if (answer === undefined) {
            return;
        }
        if (answer === COPY_COMMAND) {
            if (command) void vscode.env.clipboard.writeText(command);
            return;
        }
        if (answer === ALWAYS) {
            void this.context.workspaceState.update(ALWAYS_KEY, true);
            this.log('semantic kits are installed without asking in this workspace');
        }
        const target = this.pending.get(root);
        if (target === undefined) {
            // S3-4-21: the kit was installed meanwhile (by hand, or by another window).
            this.log('the semantic kit is no longer missing; nothing to do');
            return;
        }
        this.log('installing the semantic kit');
        this.install(target);
    }
}
