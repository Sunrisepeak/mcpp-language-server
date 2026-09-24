// The commands: select context, show module graph, restart, show logs, collect a diagnostic report.

import * as vscode from 'vscode';
import type { LanguageClient } from 'vscode-languageclient/node';
import { restoreOtherCppFeatures, turnOffOtherCppFeatures } from './conflicts';
import { describeProfile, SemanticProfile } from './status';

export interface ServerAccess {
    // The running client, or undefined when the server is not running.
    runningClient(): LanguageClient | undefined;
    restart(): Promise<void>;
    showLogs(): void;
    log(line: string): void;
}

interface ProtocolRange {
    start: { line: number; character: number };
    end: { line: number; character: number };
}

export type ModuleUnitRole =
    | 'module-interface'
    | 'module-partition-interface'
    | 'module-partition-implementation'
    | 'module-implementation'
    | 'non-module'
    | 'unknown';

export interface ModuleGraph {
    modules: { name: string; external: boolean; units: { uri: string; role: ModuleUnitRole }[] }[];
    imports: { from: string; module: string; range: ProtocolRange }[];
}

export interface ContextList {
    current: string;
    available: { id: string; label: string; profile: SemanticProfile }[];
}

interface ContextPick extends vscode.QuickPickItem {
    id?: string;
}

const CPP_LANGUAGES: readonly string[] = ['cpp', 'c'];

function errorText(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

// S3 3: cxxModules/ requests go only to a server that declared `experimental.cxxModules`.
export function declaresModules(capabilities: { experimental?: unknown } | undefined): boolean {
    const experimental = capabilities?.experimental;
    return typeof experimental === 'object' && experimental !== null && 'cxxModules' in experimental;
}

const NO_MODULE_REQUESTS = 'The language server does not offer C++ module requests.';

async function selectContext(access: ServerAccess): Promise<void> {
    const title = 'C++ Modules: Select Context';
    const client = access.runningClient();
    const editor = vscode.window.activeTextEditor;
    if (!client) {
        await vscode.window.showQuickPick([{ label: 'The C++ Modules language server is not running.' }], { title });
        return;
    }
    if (!declaresModules(client.initializeResult?.capabilities)) {
        await vscode.window.showQuickPick([{ label: NO_MODULE_REQUESTS }], { title });
        return;
    }
    if (!editor || !CPP_LANGUAGES.includes(editor.document.languageId)) {
        await vscode.window.showQuickPick([{ label: 'Open a C++ file to choose the context it is analyzed in.' }], { title });
        return;
    }
    const textDocument = { uri: client.code2ProtocolConverter.asUri(editor.document.uri) };
    let contexts: ContextList;
    try {
        contexts = await client.sendRequest<ContextList>('cxxModules/contexts', { textDocument });
    } catch (error) {
        access.log(`cxxModules/contexts failed: ${errorText(error)}`);
        await vscode.window.showQuickPick([{ label: `The contexts could not be read: ${errorText(error)}` }], { title });
        return;
    }
    const available = contexts?.available ?? [];
    if (available.length === 0) {
        await vscode.window.showQuickPick([{ label: 'This file has a single context.' }], { title });
        return;
    }
    const items: ContextPick[] = available.map((context) => ({
        id: context.id,
        label: context.label,
        description: context.id === contexts.current ? `${describeProfile(context.profile)} · current` : describeProfile(context.profile),
        detail: context.profile?.target,
    }));
    const choice = await vscode.window.showQuickPick(items, { title, placeHolder: 'The build context this file is analyzed in' });
    if (!choice?.id || choice.id === contexts.current) {
        return;
    }
    try {
        await client.sendRequest('cxxModules/setContext', { textDocument, context: choice.id });
    } catch (error) {
        access.log(`cxxModules/setContext failed: ${errorText(error)}`);
    }
}

export function renderGraph(graph: ModuleGraph, toDisplayPath: (uri: string) => string): string {
    const lines: string[] = ['# C++ Module Graph', ''];
    const modules = [...(graph?.modules ?? [])].sort((a, b) => a.name.localeCompare(b.name));
    if (modules.length === 0) {
        lines.push('No modules were found in this workspace.');
        return `${lines.join('\n')}\n`;
    }
    const importers = new Map<string, string[]>();
    for (const edge of graph.imports ?? []) {
        const list = importers.get(edge.module) ?? [];
        list.push(`${toDisplayPath(edge.from)}:${edge.range.start.line + 1}`);
        importers.set(edge.module, list);
    }
    for (const module of modules) {
        lines.push(`## ${module.name}${module.external ? ' (external)' : ''}`, '');
        for (const unit of module.units) {
            lines.push(`- ${unit.role}: ${toDisplayPath(unit.uri)}`);
        }
        const users = importers.get(module.name) ?? [];
        if (users.length > 0) {
            lines.push('', 'Imported by:', '');
            for (const user of users.sort()) {
                lines.push(`- ${user}`);
            }
        }
        lines.push('');
    }
    return `${lines.join('\n')}\n`;
}

async function showModuleGraph(access: ServerAccess): Promise<void> {
    const client = access.runningClient();
    let content: string;
    if (!client) {
        content = '# C++ Module Graph\n\nThe C++ Modules language server is not running. Run **C++ Modules: Show Logs** for details.\n';
    } else if (!declaresModules(client.initializeResult?.capabilities)) {
        content = `# C++ Module Graph\n\n${NO_MODULE_REQUESTS}\n`;
    } else {
        try {
            const graph = await client.sendRequest<ModuleGraph>('cxxModules/graph', {});
            content = renderGraph(graph, (uri) => vscode.workspace.asRelativePath(client.protocol2CodeConverter.asUri(uri), false));
        } catch (error) {
            access.log(`cxxModules/graph failed: ${errorText(error)}`);
            content = `# C++ Module Graph\n\nThe module graph could not be read: ${errorText(error)}\n`;
        }
    }
    const document = await vscode.workspace.openTextDocument({ language: 'markdown', content });
    await vscode.window.showTextDocument(document, { preview: true });
}

function withTimeout<T>(promise: Thenable<T>, milliseconds: number, what: string): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`${what} did not answer within ${milliseconds / 1000} s`)), milliseconds);
        promise.then((value) => { clearTimeout(timer); resolve(value); }, (error: unknown) => { clearTimeout(timer); reject(error); });
    });
}

// robustness design O3: what a bug report needs, in one document a person can read, copy or save. The
// server's part (cxxModules/report) comes with the extension's own: versions, settings, other C++ extensions.
async function collectReport(access: ServerAccess): Promise<void> {
    const client = access.runningClient();
    const extension = vscode.extensions.getExtension('sunrisepeak.mcpp-language-server');
    const settings = vscode.workspace.getConfiguration('mcppls');
    const report: Record<string, unknown> = {
        extension: {
            version: (extension?.packageJSON as { version?: string } | undefined)?.version,
            vscode: vscode.version,
            platform: `${process.platform}-${process.arch}`,
            otherCppExtensions: ['ms-vscode.cpptools', 'llvm-vs-code-extensions.vscode-clangd', 'mcpp-community.mcpp-vscode']
                .filter((id) => vscode.extensions.getExtension(id) !== undefined),
            settings: {
                compiler: settings.get('compiler'),
                semanticKit: settings.get('semanticKit'),
                engine: settings.get('engine'),
                aiEnabled: settings.get('ai.enabled'),
                detectConflicts: settings.get('detectConflicts'),
            },
        },
        workspaceFolders: (vscode.workspace.workspaceFolders ?? []).map((folder) => folder.uri.fsPath),
    };
    if (!client) {
        report.server = 'not running';
    } else if (!declaresModules(client.initializeResult?.capabilities)) {
        report.server = NO_MODULE_REQUESTS;
    } else {
        try {
            report.server = await withTimeout(client.sendRequest('cxxModules/report', {}), 30000, 'cxxModules/report');
        } catch (error) {
            access.log(`cxxModules/report failed: ${errorText(error)}`);
            report.server = `cxxModules/report failed: ${errorText(error)}`;
        }
    }
    const content = JSON.stringify(report, null, 2);
    const document = await vscode.workspace.openTextDocument({ language: 'json', content });
    await vscode.window.showTextDocument(document, { preview: false });
    const choice = await vscode.window.showInformationMessage(
        'C++ Modules: the diagnostic report is open. It names paths on this machine; attach it to an issue as it is or after editing.',
        'Copy to Clipboard', 'Show Logs');
    if (choice === 'Copy to Clipboard') {
        await vscode.env.clipboard.writeText(content);
    } else if (choice === 'Show Logs') {
        access.showLogs();
    }
}

// Build description design 4.4. The server runs the build tool offline, so a project whose
// dependencies are not on the machine yet cannot be described without a download. That download is
// the user's to start, and it is offered in their own terminal for a reason: a proxy set by hand in
// one terminal window is in no shell configuration and in no environment an editor can reproduce,
// so the command that works is the one the user runs where they set it.
async function runBuildToolInTerminal(access: ServerAccess): Promise<void> {
    const folder = vscode.workspace.workspaceFolders?.find((candidate) => candidate.uri.scheme === 'file');
    if (!folder) {
        void vscode.window.showWarningMessage('C++ Modules: no folder is open.');
        return;
    }
    const exists = async (name: string): Promise<boolean> => {
        try {
            await vscode.workspace.fs.stat(vscode.Uri.joinPath(folder.uri, name));
            return true;
        } catch {
            return false;
        }
    };
    let command: string | undefined;
    if (await exists('mcpp.toml')) {
        command = 'mcpp build';
    } else if (await exists('CMakeLists.txt')) {
        command = 'cmake -S . -B build';
    }
    if (!command) {
        void vscode.window.showWarningMessage('C++ Modules: this folder has no mcpp.toml or CMakeLists.txt to build.');
        return;
    }
    const choice = await vscode.window.showInformationMessage(
        `C++ Modules: run \`${command}\` to fetch what the build description needs. Your own terminal is where a proxy `
        + 'or credentials you set by hand are; the integrated terminal may not have them.',
        'Run in Integrated Terminal', 'Copy Command');
    if (choice === 'Copy Command') {
        await vscode.env.clipboard.writeText(command);
        access.log(`copied the build command: ${command}`);
        return;
    }
    if (choice !== 'Run in Integrated Terminal') return;
    const terminal = vscode.window.createTerminal({ name: 'C++ Modules build', cwd: folder.uri });
    terminal.show(true);
    terminal.sendText(command, true);
    access.log(`running the build command in a terminal: ${command}`);
    // When that terminal is gone the download either happened or it did not; reading the build
    // description again is offline and cheap, and it is the only way to find out (design 4.4).
    const closed = vscode.window.onDidCloseTerminal((gone) => {
        if (gone !== terminal) return;
        closed.dispose();
        void reloadBuildDescription(access);
    });
}

// Reads the build description again. The server bounds how often it acts on this.
export async function reloadBuildDescription(access: ServerAccess): Promise<void> {
    const client = access.runningClient();
    if (!client) return;
    try {
        await client.sendRequest('workspace/executeCommand', { command: 'mcppls.reloadBuildDescription', arguments: [] });
    } catch (error) {
        access.log(`reloading the build description failed: ${String(error)}`);
    }
}

export function registerCommands(context: vscode.ExtensionContext, access: ServerAccess): void {
    context.subscriptions.push(
        vscode.commands.registerCommand('mcppls.selectContext', () => selectContext(access)),
        vscode.commands.registerCommand('mcppls.showModuleGraph', () => showModuleGraph(access)),
        vscode.commands.registerCommand('mcppls.restartServer', () => access.restart()),
        vscode.commands.registerCommand('mcppls.showLogs', () => access.showLogs()),
        vscode.commands.registerCommand('mcppls.collectReport', () => collectReport(access)),
        vscode.commands.registerCommand('mcppls.runBuildToolInTerminal', () => runBuildToolInTerminal(access)),
        vscode.commands.registerCommand('mcppls.turnOffOtherCppFeatures', () => turnOffOtherCppFeatures(context, access.log)),
        vscode.commands.registerCommand('mcppls.restoreOtherCppFeatures', () => restoreOtherCppFeatures(context, access.log)),
    );
}
