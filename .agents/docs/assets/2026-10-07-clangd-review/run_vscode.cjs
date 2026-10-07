// node run_vscode.cjs STAGING_DIR fork|vanilla
const fs = require('fs');
const path = require('path');
const {spawnSync} = require('child_process');
const [stage, side] = process.argv.slice(2);
const source = path.join(stage, 'source/editors/vscode');
const {runTests} = require(path.join(source, 'node_modules/@vscode/test-electron'));
const editor = '/home/speak/.xlings/data/xpkgs/xim-x-code/1.132.0/code';
const extensions = fs.mkdtempSync(path.join(stage, `extensions-${side}-`));
const userData = fs.mkdtempSync(`/dev/shm/mcppls-review-${side}-`);
const cache = fs.mkdtempSync(`/dev/shm/mcppls-review-cache-${side}-`);
if (side === 'fork') {
    const result = spawnSync(path.join(path.dirname(editor), 'bin/code'), [
        '--extensions-dir', extensions, '--user-data-dir', userData,
        '--install-extension', path.join(stage, 'mcppls-0.0.12-fork-linux-x64.vsix')
    ], {stdio: 'inherit'});
    if (result.status !== 0) throw new Error('VSIX install failed');
} else {
    const original = '/home/speak/.vscode/extensions/sunrisepeak.mcpp-language-server-0.0.11-linux-x64';
    const result = spawnSync('cp', ['-al', original, extensions], {stdio: 'inherit'});
    if (result.status !== 0) throw new Error('baseline extension staging failed');
}
runTests({
    vscodeExecutablePath: editor,
    extensionDevelopmentPath: path.join(source, 'test/harness'),
    extensionTestsPath: path.join(__dirname, 'vscode_probe.cjs'),
    launchArgs: ['/home/speak/test/mcpp/qt-demo', '--extensions-dir=' + extensions,
        '--user-data-dir=' + userData, '--disable-workspace-trust', '--skip-welcome',
        '--skip-release-notes', '--disable-telemetry'],
    extensionTestsEnv: {MCPPLS_TEST: '1', MCPPLS_CACHE_DIR: cache,
        REVIEW_VSCODE_OUTPUT: path.join(stage, 'vscode-' + side + '.json')}
}).then(code => {process.exitCode = code;}).catch(error => {console.error(error); process.exitCode = 1;});
