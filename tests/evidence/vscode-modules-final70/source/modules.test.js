"use strict";
// End to end: VS Code → this extension → mcppls → clangd, on C++ modules
// without a build system. Positions refer to the fixture's src/main.cpp:
//
//   0  import std;
//   1  import hello.greet;
//   2
//   3  int main(int argc, char* argv[]) {
//   4      std::println("{}", hello::greet("mcpp"));
//   5      return 0;
//   6  }
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
const assert = __importStar(require("assert"));
const path = __importStar(require("path"));
const vscode = __importStar(require("vscode"));
const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 120_000;
const RESULT_TIMEOUT_MS = 90_000;
async function eventually(what, query, accept) {
    const deadline = Date.now() + RESULT_TIMEOUT_MS;
    let last;
    let lastError;
    while (Date.now() < deadline) {
        try {
            last = await query();
            if (accept(last)) {
                return last;
            }
        }
        catch (error) {
            lastError = error;
        }
        await new Promise((resolve) => setTimeout(resolve, 1000));
    }
    const detail = lastError instanceof Error ? lastError.message : JSON.stringify(last, undefined, 1);
    throw new Error(`${what}: no acceptable result within ${RESULT_TIMEOUT_MS} ms; last: ${detail}`);
}
function locationUris(results) {
    return (results ?? []).map((result) => ('targetUri' in result ? result.targetUri : result.uri).fsPath);
}
function hoverText(hovers) {
    const parts = [];
    for (const hover of hovers ?? []) {
        for (const content of hover.contents) {
            if (typeof content === 'string') {
                parts.push(content);
            }
            else if (content instanceof vscode.MarkdownString) {
                parts.push(content.value);
            }
            else {
                parts.push(content.value);
            }
        }
    }
    return parts.join('\n').trim();
}
function completionLabels(list) {
    return (list?.items ?? []).map((item) => (typeof item.label === 'string' ? item.label : item.label.label).trim());
}
suite('C++ modules through mcppls', function () {
    this.timeout(600_000);
    let api;
    let mainUri;
    suiteSetup(async function () {
        const extension = vscode.extensions.getExtension(EXTENSION_ID);
        assert.ok(extension, `${EXTENSION_ID} is not installed in the test instance`);
        api = await extension.activate();
        const folder = vscode.workspace.workspaceFolders?.[0];
        assert.ok(folder, 'the fixture workspace is not open');
        mainUri = vscode.Uri.joinPath(folder.uri, 'src', 'main.cpp');
        const document = await vscode.workspace.openTextDocument(mainUri);
        assert.strictEqual(document.languageId, 'cpp');
        await vscode.window.showTextDocument(document);
        // Inferred projects may legitimately report a degraded state; both are usable.
        const status = await api.waitForState(['ready', 'degraded'], READY_TIMEOUT_MS);
        console.log(`server status: ${JSON.stringify(status)}`);
    });
    test('.cppm files are C++', async () => {
        const folder = vscode.workspace.workspaceFolders[0];
        const document = await vscode.workspace.openTextDocument(vscode.Uri.joinPath(folder.uri, 'src', 'greet', 'greet.cppm'));
        assert.strictEqual(document.languageId, 'cpp');
    });
    test('definition of hello::greet lands in greet.cppm', async () => {
        const uris = await eventually('definition of hello::greet', () => vscode.commands.executeCommand('vscode.executeDefinitionProvider', mainUri, new vscode.Position(4, 31)), (results) => locationUris(results).some((uri) => path.basename(uri) === 'greet.cppm'));
        assert.ok(locationUris(uris).some((uri) => path.basename(uri) === 'greet.cppm'));
    });
    test('definition of the module name in `import hello.greet;` lands in greet.cppm', async () => {
        const uris = await eventually('definition of the module name', () => vscode.commands.executeCommand('vscode.executeDefinitionProvider', mainUri, new vscode.Position(1, 9)), (results) => locationUris(results).some((uri) => path.basename(uri) === 'greet.cppm'));
        assert.ok(locationUris(uris).some((uri) => path.basename(uri) === 'greet.cppm'));
    });
    test('hover on hello::greet has content', async () => {
        const hovers = await eventually('hover on hello::greet', () => vscode.commands.executeCommand('vscode.executeHoverProvider', mainUri, new vscode.Position(4, 31)), (results) => hoverText(results).length > 0);
        assert.ok(hoverText(hovers).length > 0);
    });
    test('references of hello::greet span main.cpp and greet.cppm', async () => {
        const accept = (results) => {
            const names = (results ?? []).map((location) => path.basename(location.uri.fsPath));
            return names.includes('main.cpp') && names.includes('greet.cppm');
        };
        const references = await eventually('references of hello::greet', () => vscode.commands.executeCommand('vscode.executeReferenceProvider', mainUri, new vscode.Position(4, 31)), accept);
        assert.ok(accept(references));
    });
    test('completion after hello:: offers greet', async () => {
        const document = await vscode.workspace.openTextDocument(mainUri);
        const edit = new vscode.WorkspaceEdit();
        edit.insert(mainUri, new vscode.Position(5, 0), '    hello::\n');
        assert.ok(await vscode.workspace.applyEdit(edit));
        try {
            const list = await eventually('completion after hello::', () => vscode.commands.executeCommand('vscode.executeCompletionItemProvider', mainUri, new vscode.Position(5, 11)), (result) => completionLabels(result).some((label) => label.startsWith('greet')));
            assert.ok(completionLabels(list).some((label) => label.startsWith('greet')));
        }
        finally {
            const revert = new vscode.WorkspaceEdit();
            revert.delete(mainUri, new vscode.Range(new vscode.Position(5, 0), new vscode.Position(6, 0)));
            await vscode.workspace.applyEdit(revert);
            await document.save();
        }
    });
    test('saving a new module export makes it available in importer completion (C7)', async () => {
        const folder = vscode.workspace.workspaceFolders[0];
        const moduleUri = vscode.Uri.joinPath(folder.uri, 'src', 'greet', 'greet.cppm');
        const moduleDocument = await vscode.workspace.openTextDocument(moduleUri);
        const mainDocument = await vscode.workspace.openTextDocument(mainUri);
        const moduleOriginal = moduleDocument.getText();
        const mainOriginal = mainDocument.getText();
        const marker = 'export namespace hello {';
        assert.ok(moduleOriginal.includes(marker));
        assert.ok(!moduleOriginal.includes('greet2'));
        const hasNewExport = (list) => (list?.items ?? []).some((item) => {
            const name = (typeof item.label === 'string' ? item.label : item.label.label).trim();
            return item.kind === vscode.CompletionItemKind.Function
                && (name === 'greet2' || name.startsWith('greet2('));
        });
        const replace = async (document, text) => {
            const edit = new vscode.WorkspaceEdit();
            edit.replace(document.uri, new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length)), text);
            assert.ok(await vscode.workspace.applyEdit(edit));
            assert.ok(await document.save());
        };
        try {
            await replace(moduleDocument, moduleOriginal.replace(marker, `${marker}\n  int greet2() { return 2; }`));
            const edit = new vscode.WorkspaceEdit();
            edit.insert(mainUri, new vscode.Position(5, 0), '    hello::\n');
            assert.ok(await vscode.workspace.applyEdit(edit));
            const list = await eventually('completion of the saved new module export', () => vscode.commands.executeCommand('vscode.executeCompletionItemProvider', mainUri, new vscode.Position(5, 11)), hasNewExport);
            assert.ok(hasNewExport(list));
        }
        finally {
            try {
                await replace(mainDocument, mainOriginal);
            }
            finally {
                await replace(moduleDocument, moduleOriginal);
            }
        }
    });
    test('the extension showed no notifications or other unsolicited UI', async () => {
        // No conflicting extension is installed in this suite (see the
        // "conflicts" scenario for that), so the real check runs and finds
        // nothing to ask about.
        assert.strictEqual(await api.conflictCheck(), 'none-found');
        assert.strictEqual(api.promptShownCount('conflict'), 0);
        assert.strictEqual(api.promptShownCount('commandLineTools'), 0);
        // What this asserts is that the extension does not INTERRUPT: no notification, no output
        // channel stealing focus, no webview, no document opened behind the person's back. Each of
        // those takes attention away from what they were doing.
        //
        // A status bar item is not in that class and is no longer counted here (cold-start plan
        // 4.2). It never takes focus, and during a cold start the alternative was worse: the state
        // lived only in a LanguageStatusItem behind the `{}` icon, so a person waiting minutes for
        // modules had to know where to look to find out that anything was happening at all. The
        // count of them is asserted separately below, so "one, always" stays enforced.
        //
        // A negative count means the counter itself could not be installed in this VS Code build,
        // which is reported rather than asserted on.
        const counters = [
            ['notificationCount', api.notificationCount()],
            ['outputChannelShowCount', api.outputChannelShowCount()],
            ['webviewPanelCount', api.webviewPanelCount()],
            ['showTextDocumentCount', api.showTextDocumentCount()],
        ];
        for (const [name, count] of counters) {
            if (count < 0) {
                console.log(`${name} could not be counted in this VS Code build`);
            }
            else {
                assert.strictEqual(count, 0, `${name} was ${count}`);
            }
        }
        // Exactly one of each of the two always-present items, and no more: a second status bar
        // item, or a second language status item, would mean something is being created per event.
        const statusBarItemCount = api.statusBarItemCount();
        if (statusBarItemCount < 0) {
            console.log('statusBarItemCount could not be counted in this VS Code build');
        }
        else {
            assert.strictEqual(statusBarItemCount, 1, `statusBarItemCount was ${statusBarItemCount}`);
        }
        const languageStatusItemCount = api.languageStatusItemCount();
        if (languageStatusItemCount < 0) {
            console.log('languageStatusItemCount could not be counted in this VS Code build');
        }
        else {
            assert.strictEqual(languageStatusItemCount, 1, `languageStatusItemCount was ${languageStatusItemCount}`);
        }
    });
    // The server logs to stderr, `[info]` for a healthy start. vscode-languageclient writes every
    // stderr line as an error unless told otherwise, which made this very session read as a wall of
    // errors; the lines must arrive at the level the server gave them.
    test('the server log reaches the output at its own level, and a working session has no errors', async () => {
        const info = api.serverLogLineCount('info');
        const warning = api.serverLogLineCount('warning');
        const error = api.serverLogLineCount('error');
        console.log(`server log lines: ${info} info, ${warning} warning, ${error} error`);
        assert.ok(info > 0, 'no server line was written at info level');
        assert.strictEqual(error, 0, `${error} server line(s) were written at error level in a session with nothing wrong`);
    });
});
//# sourceMappingURL=modules.test.js.map