// The commands: select context, show module graph, restart, show logs, collect a diagnostic report,
// export a diagnostic bundle, restart clangd, reset a workspace's cache.

import * as os from 'os';
import * as vscode from 'vscode';
import type { LanguageClient } from 'vscode-languageclient/node';
import { SETTABLE_CANDIDATES, UNSETTABLE_CANDIDATES } from './conflictCandidates';
import { restoreOtherCppFeatures, turnOffOtherCppFeatures } from './conflicts';
import { advertisesCacheReset, freedText, parseCacheResetResult, RESET_CACHE_COMMAND, SERVER_RESET_CACHE_COMMAND, sizeText } from './cacheReset';
import { OPEN_CACHE_HUB_COMMAND, REVEAL_CACHE_DIRECTORY_COMMAND, rememberCacheDetail, SERVER_SWEEP_CACHE_COMMAND, SWEEP_WORKSPACE_CACHE_COMMAND, parseSweepResult, sweepResultText } from './cacheSweep';
import { CacheDetail } from './cacheSegment';
import { openCacheHub } from './cacheHubView';
import { REPOSITORY, feedbackIssueUrl, IssueContext } from './issueUrl';
import { turnOffInWorkspace, turnOnInWorkspace } from './enable';
import { sourceOf } from './quickSuggestions';
import { RENAMED_SETTINGS, resolveRenamed, workersSetting } from './settingsRead';
import { redactJson, Who } from './redact';
import { describeProfile, SemanticProfile } from './status';

export interface ServerAccess {
    // The running client, or undefined when the server is not running.
    runningClient(): LanguageClient | undefined;
    restart(): Promise<void>;
    showLogs(): void;
    log(line: string): void;
    // The extension's own latest log lines, oldest first (the server keeps its own log in a file).
    recentLog(): string[];
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

function extensionVersion(): string | undefined {
    const extension = vscode.extensions.getExtension('sunrisepeak.mcpp-language-server');
    return (extension?.packageJSON as { version?: string } | undefined)?.version;
}

// The extension's own part of a report and of the bundle. Code-OSS builds (VSCodium, Code - OSS in
// termux, code-server) differ from Microsoft's in ways a bug report needs to know: `appName` says which
// one it is, `appHost` where it runs from (desktop, a web host), `uiKind` desktop or web.
export function extensionEnvironment(): Record<string, unknown> {
    return {
        version: extensionVersion(),
        vscode: vscode.version,
        appName: vscode.env.appName,
        appHost: vscode.env.appHost,
        uiKind: vscode.UIKind[vscode.env.uiKind],
        platform: `${process.platform}-${process.arch}`,
        remote: vscode.env.remoteName ?? null,
    };
}

// The extensions known to provide inline completions (grey text): with VS Code's own default for
// `editor.quickSuggestions` any one of them keeps the completion list closed while you type (WA-VSCODE-002).
const INLINE_COMPLETION_EXTENSIONS: readonly string[] = [
    'GitHub.copilot', 'GitHub.copilot-chat', 'Codeium.codeium', 'TabNine.tabnine-vscode', 'sourcegraph.cody-ai',
    'AmazonWebServices.amazon-q-vscode', 'Continue.continue', 'supermaven.supermaven', 'Google.geminicodeassist',
];

// What decides whether the completion list opens while a person types (0.0.8 plan E-3), as C++ files see
// it: a report of "no completion" is answered by these before anything on the server's side -- the server
// only counts the requests that reached it.
export function editorEnvironment(): Record<string, unknown> {
    const editor = vscode.workspace.getConfiguration('editor', { languageId: 'cpp' });
    const quickSuggestions = editor.inspect('quickSuggestions');
    const files = vscode.workspace.getConfiguration('files', { languageId: 'cpp' });
    return {
        quickSuggestions: {
            value: editor.get('quickSuggestions'),
            source: sourceOf(quickSuggestions),
            editorDefault: quickSuggestions?.defaultValue,
        },
        inlineSuggest: editor.get('inlineSuggest.enabled'),
        suggestOnTriggerCharacters: editor.get('suggestOnTriggerCharacters'),
        autoSave: files.get('autoSave'),
        autoSaveDelay: files.get('autoSaveDelay'),
        inlineCompletionExtensions: INLINE_COMPLETION_EXTENSIONS
            .map((id) => vscode.extensions.getExtension(id))
            .filter((extension): extension is vscode.Extension<unknown> => extension !== undefined)
            .map((extension) => ({ id: extension.id, active: extension.isActive })),
    };
}

function mcpplsSettings(): Record<string, unknown> {
    const settings = vscode.workspace.getConfiguration('mcppls');
    return {
        compiler: settings.get('compiler'),
        semanticKit: settings.get('semanticKit'),
        'engine.name': resolveRenamed(settings, RENAMED_SETTINGS[0], 'clangd'),
        'engine.workers': workersSetting(settings.get<string>('engine.workers')),
        'buildDiscovery.mode': resolveRenamed(settings, RENAMED_SETTINGS[1], 'auto'),
        buildTool: settings.get('buildTool'),
        toolEnvironment: settings.get('toolEnvironment'),
        semanticTokensModules: settings.get('semanticTokens.modules'),
        traceServer: settings.get('trace.server'),
        aiEnabled: settings.get('ai.enabled'),
        detectConflicts: settings.get('detectConflicts'),
    };
}

// The other C/C++ extensions a report of a problem needs to know about: installed and enabled, active,
// and for those with a setting for it, whether their language features are turned off.
function otherCppExtensions(): Record<string, unknown>[] {
    const described: Record<string, unknown>[] = [];
    const describe = (extensionId: string, featuresOff?: boolean) => {
        const extension = vscode.extensions.getExtension(extensionId);
        if (!extension) return;
        described.push({
            id: extensionId,
            version: (extension.packageJSON as { version?: string } | undefined)?.version,
            active: extension.isActive,
            ...(featuresOff === undefined ? {} : { languageFeaturesOff: featuresOff }),
        });
    };
    for (const candidate of SETTABLE_CANDIDATES) {
        describe(candidate.extensionId, vscode.workspace.getConfiguration(candidate.section).get(candidate.key) === candidate.disabledValue);
    }
    for (const candidate of UNSETTABLE_CANDIDATES) {
        describe(candidate.extensionId);
    }
    describe('mcpp-community.mcpp-vscode');
    return described;
}

// robustness design O3: what a bug report needs, in one document a person can read, copy or save. The
// server's part (cxxModules/report) comes with the extension's own: versions, settings, other C++ extensions.
// The server redacts its part (S3-5.5-3); the extension's own is redacted here by the same rules.
async function collectReport(access: ServerAccess): Promise<void> {
    const client = access.runningClient();
    const report: Record<string, unknown> = redactJson({
        extension: {
            ...extensionEnvironment(),
            otherCppExtensions: otherCppExtensions(),
            settings: mcpplsSettings(),
            editor: editorEnvironment(),
        },
        workspaceFolders: (vscode.workspace.workspaceFolders ?? []).map((folder) => folder.uri.fsPath),
    }, whoAmI());
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
        'C++ Modules: the diagnostic report is open. Your user name, home directory, host name and anything that looks like a '
        + 'secret were replaced; the project\'s own paths are kept. Export Diagnostic Bundle packs it with the logs and the environment.',
        'Copy to Clipboard', 'Export Diagnostic Bundle', 'Show Logs');
    if (choice === 'Copy to Clipboard') {
        await vscode.env.clipboard.writeText(content);
    } else if (choice === 'Export Diagnostic Bundle') {
        await exportDiagnosticBundle(access);
    } else if (choice === 'Show Logs') {
        access.showLogs();
    }
}

interface BundleWritten {
    path: string;
    bytes: number;
    redactions?: Record<string, number>;
}

// Issue #23 fix plan F18: one zip with what a report of a problem needs -- the server's report, the
// environment, the logs of the last sessions, the incidents, the engine databases -- written by the
// server with user names, paths and secrets replaced, and never uploaded. When the server's check
// finds something its rules left, nothing is written, and hiding the project's paths too is offered.
//
// Returns the path of the bundle written, or undefined when none was. `quiet`: the caller tells the
// person about the file itself (the unrecoverable-error notification does), so no message of its own.
export async function exportDiagnosticBundle(access: ServerAccess, hideProjectPaths = false, quiet = false): Promise<string | undefined> {
    const client = access.runningClient();
    if (!client) {
        void vscode.window.showWarningMessage(
            'C++ Modules: the language server is not running. `mcppls report --bundle <file.zip>` in a terminal writes the same bundle.');
        return;
    }
    const argument = {
        hideProjectPaths,
        client: {
            extension: extensionEnvironment(),
            otherCppExtensions: otherCppExtensions(),
            settings: mcpplsSettings(),
            editor: editorEnvironment(),
            log: access.recentLog().join('\n'),
        },
    };
    let written: BundleWritten;
    try {
        written = await vscode.window.withProgress(
            { location: vscode.ProgressLocation.Notification, title: 'C++ Modules: writing a diagnostic bundle' },
            () => withTimeout(client.sendRequest<BundleWritten>('workspace/executeCommand', { command: 'mcppls.exportBundle', arguments: [argument] }),
                120000, 'mcppls.exportBundle'));
    } catch (error) {
        const message = errorText(error);
        access.log(`mcppls.exportBundle failed: ${message}`);
        if (quiet) return undefined;
        const offer = hideProjectPaths ? [] : ['Retry with Project Paths Hidden'];
        const choice = await vscode.window.showWarningMessage(`C++ Modules: no diagnostic bundle was written. ${message}`, ...offer, 'Show Logs');
        if (choice === 'Retry with Project Paths Hidden') {
            return exportDiagnosticBundle(access, true);
        } else if (choice === 'Show Logs') {
            access.showLogs();
        }
        return undefined;
    }
    access.log(`diagnostic bundle written: ${written.path} (${written.bytes} bytes)`);
    if (quiet) return written.path;
    const choice = await vscode.window.showInformationMessage(
        `C++ Modules: diagnostic bundle written (${sizeText(written.bytes)}). Your user name, home directory, host name and secrets were `
        + 'replaced; nothing was uploaded. Attach it to an issue if you choose to.',
        'Reveal in Folder', 'Copy Path');
    if (choice === 'Reveal in Folder') {
        await vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(written.path));
    } else if (choice === 'Copy Path') {
        await vscode.env.clipboard.writeText(written.path);
    }
    return written.path;
}

// Issue #23 fix plan F14: clangd restarted now, whatever its restart budget says; the server counts it
// as the user's own restart, not against that budget. Nothing else about the session changes.
async function restartClangd(access: ServerAccess): Promise<void> {
    const client = access.runningClient();
    if (!client) {
        void vscode.window.showWarningMessage('C++ Modules: the language server is not running.');
        return;
    }
    try {
        await client.sendRequest('workspace/executeCommand', { command: 'mcppls.restartEngine', arguments: [] });
        access.log('clangd restart requested');
    } catch (error) {
        access.log(`mcppls.restartEngine failed: ${errorText(error)}`);
        void vscode.window.showWarningMessage(`C++ Modules: clangd could not be restarted: ${errorText(error)}`);
    }
}

// 0.0.7 plan C-1: stop the engine, delete this workspace's cache (the logs stay), plan again, start again
// -- what `mcppls cache --clean <name>` does, without leaving the editor and without knowing where the
// cache is. The server does the deleting: the extension knows no directory of its own, so a server that
// does not list the command is asked to be updated instead. Returns the server's answer, for tests.
async function pickWorkspaceFolder(): Promise<vscode.WorkspaceFolder | undefined> {
    const folders = vscode.workspace.workspaceFolders ?? [];
    const editor = vscode.window.activeTextEditor;
    const ofEditor = editor ? vscode.workspace.getWorkspaceFolder(editor.document.uri) : undefined;
    if (ofEditor) return ofEditor;
    if (folders.length <= 1) return folders[0];
    const choice = await vscode.window.showQuickPick(
        folders.map((folder) => ({ label: folder.name, description: folder.uri.fsPath, folder })),
        { title: 'C++ Modules: Reset This Workspace\'s Cache', placeHolder: 'The workspace folder whose cache is reset' });
    return choice?.folder;
}

export async function resetWorkspaceCache(access: ServerAccess): Promise<unknown> {
    const client = access.runningClient();
    if (!client) {
        void vscode.window.showWarningMessage('C++ Modules: the language server is not running. Restart it first, or close the editor and run `mcppls cache --clean <name>` in a terminal.');
        return undefined;
    }
    if (!advertisesCacheReset(client.initializeResult?.capabilities)) {
        void vscode.window.showWarningMessage(
            'C++ Modules: this language server cannot reset a workspace\'s cache. Update the extension, or close the editor and run `mcppls cache --clean <name>` in a terminal.');
        return undefined;
    }
    const folder = await pickWorkspaceFolder();
    if (!folder) {
        void vscode.window.showWarningMessage('C++ Modules: no workspace folder is open.');
        return undefined;
    }
    const root = client.code2ProtocolConverter.asUri(folder.uri);
    let answer: unknown;
    try {
        answer = await vscode.window.withProgress(
            { location: vscode.ProgressLocation.Notification, title: `C++ Modules: resetting the cache of ${folder.name}` },
            () => withTimeout(client.sendRequest('workspace/executeCommand', { command: SERVER_RESET_CACHE_COMMAND, arguments: [{ root }] }),
                10 * 60 * 1000, SERVER_RESET_CACHE_COMMAND));
    } catch (error) {
        const message = errorText(error);
        access.log(`${SERVER_RESET_CACHE_COMMAND} failed: ${message}`);
        // Not awaited: the command is done, whether or not anyone answers the notification.
        void vscode.window.showWarningMessage(`C++ Modules: the cache was not reset. ${message}`, 'Show Logs').then((choice) => {
            if (choice === 'Show Logs') access.showLogs();
        });
        return undefined;
    }
    const result = parseCacheResetResult(answer);
    access.log(`workspace cache reset: ${folder.uri.toString()} (${result.freedBytes} bytes freed, ok=${String(result.ok)})`);
    void vscode.window.showInformationMessage(freedText(result));
    return answer;
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
    } else if (await exists('xmake.lua')) {
        command = 'xmake';
    } else if (await exists('meson.build')) {
        command = 'meson setup build';
    }
    if (!command) {
        void vscode.window.showWarningMessage('C++ Modules: this folder has no mcpp.toml, CMakeLists.txt, xmake.lua or meson.build to build.');
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

// ---- the cache (0.0.10 plan C-13) ------------------------------------------------------------

async function fetchCacheDetail(access: ServerAccess): Promise<CacheDetail | undefined> {
    const client = access.runningClient();
    if (!client) return undefined;
    try {
        const answer = (await client.sendRequest('cxxModules/cache', {})) as { roots?: CacheDetail[] } | undefined;
        const detail = answer?.roots?.[0];
        if (detail) rememberCacheDetail(detail);
        return detail;
    } catch {
        return undefined;   // an older server answers MethodNotFound; the coarse numbers still work
    }
}

export async function openCachePanel(access: ServerAccess): Promise<void> {
    // The hub fetches its own detail; the status bar carries the coarse numbers on its side.
    await openCacheHub({ client: access.runningClient(), coarse: undefined });
}

export async function sweepWorkspaceCache(access: ServerAccess): Promise<unknown> {
    const client = access.runningClient();
    if (!client) {
        void vscode.window.showWarningMessage('The C++ Modules server is not running; there is nothing to sweep.');
        return undefined;
    }
    const answer = await client.sendRequest('workspace/executeCommand', { command: SERVER_SWEEP_CACHE_COMMAND, arguments: [{ dryRun: false }] });
    const result = parseSweepResult(answer);
    void vscode.window.showInformationMessage(sweepResultText(result));
    return answer;
}

export async function copyAgentPrompt(access: ServerAccess): Promise<string | undefined> {
    const detail = await fetchCacheDetail(access);
    const prompt = detail?.prompts?.agent;
    if (!prompt) {
        void vscode.window.showWarningMessage('No agent prompt is available: the server does not carry one (older server?).');
        return undefined;
    }
    await vscode.env.clipboard.writeText(prompt);
    void vscode.window.showInformationMessage('已复制 ✓ 粘给本地 agent——日志不会离开本机');
    return prompt;
}

export async function copyIssuePrompt(access: ServerAccess): Promise<string | undefined> {
    const detail = await fetchCacheDetail(access);
    const prompt = detail?.prompts?.issue;
    if (!prompt) {
        void vscode.window.showWarningMessage('No issue prompt is available: the server does not carry one (older server?).');
        return undefined;
    }
    await vscode.env.clipboard.writeText(prompt);
    void vscode.window.showInformationMessage('已复制 issue 提示词 ✓ 先给人看，同意后再发');
    return prompt;
}

export async function revealCacheDirectory(access: ServerAccess, which = 'cache'): Promise<void> {
    const detail = await fetchCacheDetail(access);
    const path = which === 'logs' ? detail?.paths.logDirectory : detail?.paths.cacheRoot;
    if (!path) {
        access.showLogs();
        return;
    }
    await vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(path));
}

function issueContext(): IssueContext {
    const extension = vscode.extensions.getExtension('sunrisepeak.mcpp-language-server');
    return {
        code: 'cache',
        message: 'the module cache grew beyond its budget',
        extensionVersion: extension?.packageJSON?.version as string | undefined,
        appName: 'VS Code',
        editorVersion: vscode.version,
        platform: process.platform,
        arch: process.arch,
    };
}

export async function newCacheIssue(access: ServerAccess): Promise<void> {
    // Fetched for its side effect (the detail is remembered for the next card) and to fail loudly
    // when there is no server to ask.
    await fetchCacheDetail(access);
    await vscode.env.openExternal(vscode.Uri.parse(feedbackIssueUrl(issueContext())));
}

export async function openRepository(): Promise<void> {
    await vscode.env.openExternal(vscode.Uri.parse(REPOSITORY));
}

export async function openCacheSettings(): Promise<void> {
    await vscode.commands.executeCommand('workbench.action.openSettings', '@ext:sunrisepeak.mcpp-language-server cache');
}

export function registerCommands(context: vscode.ExtensionContext, access: ServerAccess): void {
    context.subscriptions.push(
        vscode.commands.registerCommand('mcppls.selectContext', () => selectContext(access)),
        vscode.commands.registerCommand('mcppls.showModuleGraph', () => showModuleGraph(access)),
        vscode.commands.registerCommand('mcppls.restartServer', () => access.restart()),
        vscode.commands.registerCommand('mcppls.showLogs', () => access.showLogs()),
        vscode.commands.registerCommand('mcppls.collectReport', () => collectReport(access)),
        vscode.commands.registerCommand('mcppls.exportDiagnosticBundle', () => exportDiagnosticBundle(access)),
        vscode.commands.registerCommand('mcppls.restartClangd', () => restartClangd(access)),
        vscode.commands.registerCommand(RESET_CACHE_COMMAND, () => resetWorkspaceCache(access)),
        vscode.commands.registerCommand(OPEN_CACHE_HUB_COMMAND, () => openCachePanel(access)),
        vscode.commands.registerCommand(SWEEP_WORKSPACE_CACHE_COMMAND, () => sweepWorkspaceCache(access)),
        vscode.commands.registerCommand('mcppls.copyAgentPrompt', () => copyAgentPrompt(access)),
        vscode.commands.registerCommand('mcppls.copyIssuePrompt', () => copyIssuePrompt(access)),
        vscode.commands.registerCommand(REVEAL_CACHE_DIRECTORY_COMMAND, (which?: string) => revealCacheDirectory(access, which)),
        vscode.commands.registerCommand('mcppls.newCacheIssue', () => newCacheIssue(access)),
        vscode.commands.registerCommand('mcppls.openRepository', () => openRepository()),
        vscode.commands.registerCommand('mcppls.openCacheSettings', () => openCacheSettings()),
        vscode.commands.registerCommand('mcppls.turnOffInWorkspace', () => turnOffInWorkspace(access.log)),
        vscode.commands.registerCommand('mcppls.turnOnInWorkspace', () => turnOnInWorkspace(access.log)),
        vscode.commands.registerCommand('mcppls.runBuildToolInTerminal', () => runBuildToolInTerminal(access)),
        vscode.commands.registerCommand('mcppls.turnOffOtherCppFeatures', () => turnOffOtherCppFeatures(context, access.log)),
        vscode.commands.registerCommand('mcppls.restoreOtherCppFeatures', () => restoreOtherCppFeatures(context, access.log)),
    );
}
