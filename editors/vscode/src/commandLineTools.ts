// The macOS "Command Line Tools" prompt (design 16, plan W5.4): the server
// reports state 'degraded' with an issue coded sdk-missing when it cannot
// find the macOS SDK. This is one of the two situations the extension is
// allowed to ask the user about (the other is conflicts.ts), and like that
// one, it asks at most once per machine, recording the answer in
// globalState rather than workspaceState: a missing SDK is a fact about the
// machine, not about any one workspace.

import { spawn } from 'child_process';
import * as vscode from 'vscode';
import { askOnce } from './prompt';
import type { CxxModulesStatus, IssueCommand } from './status';

export const INSTALL_COMMAND_ID = 'mcppls.installCommandLineTools';
export const ASKED_KEY = 'mcppls.commandLineToolsAsked';
const SDK_MISSING_CODE = 'sdk-missing';
export const INSTALL = 'Install';
export const NOT_NOW = 'Not now';

const TEST_MODE = process.env.MCPPLS_TEST === '1';

function errorText(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

function hasSdkMissing(status: CxxModulesStatus): boolean {
    return (status.issues ?? []).some((issue) => issue.code === SDK_MISSING_CODE);
}

// The server is expected to attach a command to its sdk-missing issue, but
// at the time of writing src/server/session.cpp does not yet. Fill one in
// here so the language status item's fix-it action works either way, without
// teaching status.ts (which stays generic over issue codes) about this one.
export function withInstallCommandFallback(status: CxxModulesStatus): CxxModulesStatus {
    const issues = status.issues;
    if (!issues || !issues.some((issue) => issue.code === SDK_MISSING_CODE && !issue.command)) {
        return status;
    }
    const command: IssueCommand = { title: 'Install Command Line Tools', command: INSTALL_COMMAND_ID };
    return {
        ...status,
        issues: issues.map((issue) => (issue.code === SDK_MISSING_CODE && !issue.command ? { ...issue, command } : issue)),
    };
}

export class CommandLineToolsController {
    private askedThisSession = false;
    private installInvocations = 0;

    constructor(private readonly context: vscode.ExtensionContext, private readonly log: (line: string) => void) {
        context.subscriptions.push(vscode.commands.registerCommand(INSTALL_COMMAND_ID, () => this.install()));
    }

    // Called for every cxxModules/status notification from the running server.
    onStatus(status: CxxModulesStatus): void {
        if (!hasSdkMissing(status) || this.askedThisSession || this.context.globalState.get<boolean>(ASKED_KEY, false)) {
            return;
        }
        // Both guards are set before the answer arrives: a second status
        // notification (a restart, or another update while this one is
        // in flight) must not open a second prompt while the first is still
        // pending on a reply.
        this.askedThisSession = true;
        void this.context.globalState.update(ASKED_KEY, true);
        void askOnce(
            'commandLineTools',
            'C++ Modules could not find the macOS SDK, so some standard library features are unavailable. Install the Command Line Tools?',
            INSTALL,
            NOT_NOW,
        ).then((answer) => {
            if (answer === INSTALL) {
                this.install();
            }
        });
    }

    // Also reachable directly from the language status item's fix-it command
    // and from the command palette, regardless of whether the prompt above
    // ever ran.
    install(): void {
        if (process.platform !== 'darwin') {
            this.log('Install Command Line Tools: only applicable on macOS.');
            return;
        }
        if (TEST_MODE) {
            // Never actually launch the installer from an automated test.
            this.installInvocations += 1;
            this.log('Install Command Line Tools: would run "xcode-select --install" (not run: test mode).');
            return;
        }
        try {
            const child = spawn('xcode-select', ['--install'], { detached: true, stdio: 'ignore' });
            child.unref();
            this.log('Running "xcode-select --install".');
        } catch (error) {
            this.log(`Could not start "xcode-select --install": ${errorText(error)}`);
        }
    }

    installInvocationCount(): number {
        return this.installInvocations;
    }
}
