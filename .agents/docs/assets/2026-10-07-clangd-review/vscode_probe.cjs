// Executed in the real VS Code extension host with the installed VSIX.
// The real qt-demo is opened; all injected text is reverted without saving.
const vscode = require('vscode');
const fs = require('fs');
const crypto = require('crypto');
const path = require('path');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const labels = list => (list?.items ?? []).map(item => typeof item.label === 'string' ? item.label : item.label.label);

exports.run = async () => {
    const extension = vscode.extensions.getExtension('sunrisepeak.mcpp-language-server');
    if (!extension) throw new Error('installed extension missing');
    const api = await extension.activate();
    const folder = vscode.workspace.workspaceFolders[0].uri;
    const uri = vscode.Uri.joinPath(folder, 'src/main.cpp');
    const document = await vscode.workspace.openTextDocument(uri);
    await vscode.window.showTextDocument(document);
    const raw = document.getText();
    const hash = () => crypto.createHash('sha256').update(fs.readFileSync(uri.fsPath)).digest('hex');
    const beforeHash = hash();
    const status = await api.waitForState(['ready', 'degraded'], 180000);
    await delay(5000);
    const payloadRoot = path.join(extension.extensionPath, 'payload');
    const manifest = JSON.parse(fs.readFileSync(path.join(payloadRoot, 'payload.json'), 'utf8'));
    const observations = [];
    try {
        for (const prefix of ['nlohmann::j', 'std::ve', 'cli.', 'cli.ad']) {
            const marker = '    cli.process(app);';
            const samples = [];
            for (let round = 0; round < 10; round++) {
                const injection = '\n    ' + prefix + '\n    // review round ' + round;
                const text = raw.replace(marker, marker + injection);
                const edit = new vscode.WorkspaceEdit();
                edit.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), text);
                await vscode.workspace.applyEdit(edit);
                const offset = text.indexOf('\n    ' + prefix) + ('\n    ' + prefix).length;
                await delay(350);
                const start = performance.now();
                const list = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', uri, document.positionAt(offset));
                samples.push({ms: Math.round((performance.now() - start) * 10) / 10, labels: labels(list), incomplete: list?.isIncomplete});
            }
            const times = samples.slice(1).map(s => s.ms).sort((a,b) => a-b);
            observations.push({prefix, p50: times[Math.floor(times.length / 2)], p95: times[Math.ceil(times.length * .95) - 1], samples});
        }
        const hover = await vscode.commands.executeCommand('vscode.executeHoverProvider', uri, new vscode.Position(23, 9));
        const definition = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', uri, new vscode.Position(0, 12));
        const output = {
            vscode: vscode.version, environment: api.environment(), extensionPath: extension.extensionPath,
            extensionVersion: extension.packageJSON.version, payload: manifest, status,
            observations, hoverCount: hover?.length ?? 0, definitionCount: definition?.length ?? 0,
            finalStatus: api.lastStatus(), notifications: api.notificationCount(),
            clangdSha256: crypto.createHash('sha256').update(fs.readFileSync(path.join(payloadRoot, 'clangd/bin/clangd'))).digest('hex'),
            mainFileUnchanged: beforeHash === hash(),
        };
        fs.writeFileSync(process.env.REVIEW_VSCODE_OUTPUT, JSON.stringify(output, null, 2));
        console.log('REVIEW_RESULT ' + JSON.stringify({version: output.extensionVersion, observations: observations.map(({prefix,p50,p95}) => ({prefix,p50,p95})), mainFileUnchanged: output.mainFileUnchanged}));
    } finally {
        const edit = new vscode.WorkspaceEdit();
        edit.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), raw);
        await vscode.workspace.applyEdit(edit);
        await vscode.commands.executeCommand('workbench.action.revertAndCloseActiveEditor');
    }
};
