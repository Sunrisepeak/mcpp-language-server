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

import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';
import type { TestApi } from '../../src/extension';

const EXTENSION_ID = 'sunrisepeak.mcpp-language-server';
const READY_TIMEOUT_MS = 120_000;
const RESULT_TIMEOUT_MS = 90_000;

async function eventually<T>(what: string, query: () => Thenable<T>, accept: (value: T) => boolean): Promise<T> {
    const deadline = Date.now() + RESULT_TIMEOUT_MS;
    let last: T | undefined;
    let lastError: unknown;
    while (Date.now() < deadline) {
        try {
            last = await query();
            if (accept(last)) {
                return last;
            }
        } catch (error) {
            lastError = error;
        }
        await new Promise((resolve) => setTimeout(resolve, 1000));
    }
    const detail = lastError instanceof Error ? lastError.message : JSON.stringify(last, undefined, 1);
    throw new Error(`${what}: no acceptable result within ${RESULT_TIMEOUT_MS} ms; last: ${detail}`);
}

function locationUris(results: (vscode.Location | vscode.LocationLink)[] | undefined): string[] {
    return (results ?? []).map((result) => ('targetUri' in result ? result.targetUri : result.uri).fsPath);
}

function hoverText(hovers: vscode.Hover[] | undefined): string {
    const parts: string[] = [];
    for (const hover of hovers ?? []) {
        for (const content of hover.contents) {
            if (typeof content === 'string') {
                parts.push(content);
            } else if (content instanceof vscode.MarkdownString) {
                parts.push(content.value);
            } else {
                parts.push((content as { value: string }).value);
            }
        }
    }
    return parts.join('\n').trim();
}

function completionLabels(list: vscode.CompletionList | undefined): string[] {
    return (list?.items ?? []).map((item) => (typeof item.label === 'string' ? item.label : item.label.label).trim());
}

suite('C++ modules through mcppls', function () {
    this.timeout(600_000);

    let api: TestApi;
    let mainUri: vscode.Uri;

    suiteSetup(async function () {
        const extension = vscode.extensions.getExtension<TestApi>(EXTENSION_ID);
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
        const folder = vscode.workspace.workspaceFolders![0];
        const document = await vscode.workspace.openTextDocument(vscode.Uri.joinPath(folder.uri, 'src', 'greet', 'greet.cppm'));
        assert.strictEqual(document.languageId, 'cpp');
    });

    test('definition of hello::greet lands in greet.cppm', async () => {
        const uris = await eventually('definition of hello::greet',
            () => vscode.commands.executeCommand<(vscode.Location | vscode.LocationLink)[]>(
                'vscode.executeDefinitionProvider', mainUri, new vscode.Position(4, 31)),
            (results) => locationUris(results).some((uri) => path.basename(uri) === 'greet.cppm'));
        assert.ok(locationUris(uris).some((uri) => path.basename(uri) === 'greet.cppm'));
    });

    test('definition of the module name in `import hello.greet;` lands in greet.cppm', async () => {
        const uris = await eventually('definition of the module name',
            () => vscode.commands.executeCommand<(vscode.Location | vscode.LocationLink)[]>(
                'vscode.executeDefinitionProvider', mainUri, new vscode.Position(1, 9)),
            (results) => locationUris(results).some((uri) => path.basename(uri) === 'greet.cppm'));
        assert.ok(locationUris(uris).some((uri) => path.basename(uri) === 'greet.cppm'));
    });

    test('hover on hello::greet has content', async () => {
        const hovers = await eventually('hover on hello::greet',
            () => vscode.commands.executeCommand<vscode.Hover[]>('vscode.executeHoverProvider', mainUri, new vscode.Position(4, 31)),
            (results) => hoverText(results).length > 0);
        assert.ok(hoverText(hovers).length > 0);
    });

    test('references of hello::greet span main.cpp and greet.cppm', async () => {
        const accept = (results: vscode.Location[] | undefined): boolean => {
            const names = (results ?? []).map((location) => path.basename(location.uri.fsPath));
            return names.includes('main.cpp') && names.includes('greet.cppm');
        };
        const references = await eventually('references of hello::greet',
            () => vscode.commands.executeCommand<vscode.Location[]>('vscode.executeReferenceProvider', mainUri, new vscode.Position(4, 31)),
            accept);
        assert.ok(accept(references));
    });

    test('completion after hello:: offers greet', async () => {
        const document = await vscode.workspace.openTextDocument(mainUri);
        const edit = new vscode.WorkspaceEdit();
        edit.insert(mainUri, new vscode.Position(5, 0), '    hello::\n');
        assert.ok(await vscode.workspace.applyEdit(edit));
        try {
            const list = await eventually('completion after hello::',
                () => vscode.commands.executeCommand<vscode.CompletionList>(
                    'vscode.executeCompletionItemProvider', mainUri, new vscode.Position(5, 11)),
                (result) => completionLabels(result).some((label) => label.startsWith('greet')));
            assert.ok(completionLabels(list).some((label) => label.startsWith('greet')));
        } finally {
            const revert = new vscode.WorkspaceEdit();
            revert.delete(mainUri, new vscode.Range(new vscode.Position(5, 0), new vscode.Position(6, 0)));
            await vscode.workspace.applyEdit(revert);
            await document.save();
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
        const counters: [string, number][] = [
            ['notificationCount', api.notificationCount()],
            ['outputChannelShowCount', api.outputChannelShowCount()],
            ['webviewPanelCount', api.webviewPanelCount()],
            ['showTextDocumentCount', api.showTextDocumentCount()],
        ];
        for (const [name, count] of counters) {
            if (count < 0) {
                console.log(`${name} could not be counted in this VS Code build`);
            } else {
                assert.strictEqual(count, 0, `${name} was ${count}`);
            }
        }
        // Exactly one of each of the two always-present items, and no more: a second status bar
        // item, or a second language status item, would mean something is being created per event.
        const statusBarItemCount = api.statusBarItemCount();
        if (statusBarItemCount < 0) {
            console.log('statusBarItemCount could not be counted in this VS Code build');
        } else {
            assert.strictEqual(statusBarItemCount, 1, `statusBarItemCount was ${statusBarItemCount}`);
        }
        const languageStatusItemCount = api.languageStatusItemCount();
        if (languageStatusItemCount < 0) {
            console.log('languageStatusItemCount could not be counted in this VS Code build');
        } else {
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
