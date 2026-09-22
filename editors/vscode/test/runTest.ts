// Runs the end-to-end suite in a downloaded VS Code against a copy of the
// `inferred` conformance fixture: C++ modules with no build system.
//
// Modes (development path, the default, vs. the packaged VSIX):
//   MCPPLS_E2E_VSIX=<path>       Test the packaged extension: install this
//                                  .vsix (built with `vsce package`) into a
//                                  fresh --extensions-dir and load the tests
//                                  through test/harness rather than loading
//                                  the extension from source. This is the
//                                  form CI runs (W6.3); omit it for fast
//                                  local iteration against the source tree.
//   MCPPLS_E2E_SCENARIO=stress    Instead of the main suite, seeded random use
//                                  (test/suite-stress): MCPPLS_STRESS_ACTIONS,
//                                  _SEED, _REQUEST_MS and _P90_MS tune it.
//   MCPPLS_E2E_SCENARIO=conflicts
//                                  Instead of the main suite, run the
//                                  cpptools/clangd conflict-detection
//                                  scenario (W6.4): requires
//                                  MCPPLS_E2E_VSIX, since it needs the
//                                  stub extensions genuinely installed
//                                  alongside the real one. Defaults to the
//                                  main suite.
//
// Payload / server selection (read by the extension itself, see src/payload.ts):
//   MCPPLS_PAYLOAD=<assembled payload>   (or a payload/ directory in this extension)
//   MCPPLS_SERVER=<mcppls executable>  (optional; overrides payload/bin)
//
// Other:
//   VSCODE_TEST_VERSION=stable      (optional)
//   MCPPLS_CACHE_DIR=<dir>        Reuse a server cache directory across runs.
//   MCPPLS_E2E_ONLY=<glob>        Run only the main suite's test files matching this glob, for
//                                  example sdk-missing.test.js on a clean macOS machine.
//   MCPPLS_E2E_EXPECT_SDK_MISSING=1
//                                  Tells test/suite/sdk-missing.test.ts to
//                                  run its assertions instead of skipping;
//                                  only meaningful on a clean-machine job
//                                  that has hidden the macOS Command Line
//                                  Tools before `npm test` runs there.
//
//   npm run compile && npm test            (Linux without a display: xvfb-run -a npm test)

import { spawnSync } from 'child_process';
import * as crypto from 'crypto';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { downloadAndUnzipVSCode, resolveCliArgsFromVSCodeExecutablePath, runTests } from '@vscode/test-electron';
// From conflictAnswers.ts, not conflicts.ts: this file is a plain Node
// process with no VS Code extension host, and conflicts.ts (unlike
// conflictAnswers.ts) requires('vscode') at module load time.
import { DISABLE, KEEP } from '../src/conflictAnswers';

// __dirname at runtime is out/test (this file compiles to out/test/runTest.js).
const EXTENSION_ROOT = path.resolve(__dirname, '..', '..');
const REPO_ROOT = path.resolve(EXTENSION_ROOT, '..', '..');
const BUILD_FILES = ['mcpp.toml', 'mcpp.lock', 'CMakeLists.txt', 'compile_commands.json', 'build_database.json', 'target', 'build', '.cache', 'scenario.json'];

function prepareWorkspace(): string {
    const candidates = [
        path.join(REPO_ROOT, 'conformance', 'fixtures', 'inferred'),
        path.join(REPO_ROOT, '.agents', 'docs', 'assets', '2026-09-13-modules-lsp-spike', 'fixture'),
    ];
    const fixture = candidates.find((candidate) => fs.existsSync(path.join(candidate, 'src', 'main.cpp')));
    if (!fixture) {
        throw new Error(`No fixture found; looked in:\n${candidates.join('\n')}`);
    }
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-e2e-'));
    fs.cpSync(fixture, workspace, { recursive: true });
    // The inferred fixture has no build system; remove anything that would make it another kind.
    for (const name of BUILD_FILES) {
        fs.rmSync(path.join(workspace, name), { recursive: true, force: true });
    }
    return workspace;
}

// --- Workspace-unchanged check (W6.2) --------------------------------------
//
// Hashing has to happen from outside the VS Code process, both before it
// starts and after it exits: activation can be triggered by VS Code itself
// as soon as it opens a workspace matching an activationEvent (e.g. a
// .cppm file), which can happen before any of our own test code runs, so
// "before activation" only has an unambiguous meaning if it is captured
// before VS Code is even launched.

function hashWorkspace(workspace: string): Map<string, string> {
    const result = new Map<string, string>();
    const walk = (dir: string): void => {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            const full = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                walk(full);
            } else if (entry.isFile()) {
                const relative = path.relative(workspace, full).split(path.sep).join('/');
                result.set(relative, crypto.createHash('sha256').update(fs.readFileSync(full)).digest('hex'));
            }
        }
    };
    walk(workspace);
    return result;
}

// allowedNewFiles: paths (relative, '/'-separated) that may appear that were
// not present before -- used by the conflicts scenario, which is expected to
// write .vscode/settings.json. Anything else appearing, changing, or
// disappearing is a failure; nothing is ignored for the plain suite.
function assertWorkspaceUnchanged(workspace: string, before: Map<string, string>, allowedNewFiles: readonly string[] = []): void {
    const after = hashWorkspace(workspace);
    const allowed = new Set(allowedNewFiles);
    const problems: string[] = [];
    for (const [file, hash] of after) {
        const previous = before.get(file);
        if (previous === undefined) {
            if (!allowed.has(file)) {
                problems.push(`created: ${file}`);
            }
        } else if (previous !== hash) {
            problems.push(`modified: ${file}`);
        }
    }
    for (const file of before.keys()) {
        if (!after.has(file)) {
            problems.push(`deleted: ${file}`);
        }
    }
    if (problems.length > 0) {
        throw new Error(`The workspace changed during the end-to-end suite:\n${problems.join('\n')}`);
    }
}

// --- VSIX-form support (W6.3) -----------------------------------------------

// Packages one of the test/stubs/* directories with vsce, the same way CI
// packages the real extension, and returns the produced .vsix's path.
// Invoked as `node <vsce's bin script> package ...` rather than through npx:
// require.resolve follows normal node_modules lookup from this file's own
// location, so it finds this project's devDependency without depending on
// npx's own (platform-varying) shell resolution.
function packageStub(stubDir: string): string {
    const manifest = JSON.parse(fs.readFileSync(path.join(stubDir, 'package.json'), 'utf8')) as { name: string; version: string };
    const outDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-stub-'));
    const outFile = path.join(outDir, `${manifest.name}-${manifest.version}.vsix`);
    const vsceScript = require.resolve('@vscode/vsce/vsce');
    // vsce package needs a publisher + version (both present) and either a
    // repository or --allow-missing-repository, and either a LICENSE file or
    // --skip-license; the stubs are deliberately minimal and rely on the flags.
    const result = spawnSync(process.execPath, [vsceScript, 'package', '--allow-missing-repository', '--skip-license', '--out', outFile], {
        cwd: stubDir,
        encoding: 'utf-8',
    });
    if (result.status !== 0) {
        throw new Error(`Packaging the stub extension in ${stubDir} failed:\n${result.stdout}\n${result.stderr}`);
    }
    return outFile;
}

// Installs one or more .vsix files into a fresh extensions-dir, using the
// documented resolveCliArgsFromVSCodeExecutablePath + spawnSync recipe.
// reuseMachineInstall: true here only suppresses that helper's own default
// --extensions-dir/--user-data-dir (meant for "use my real VS Code
// profile"); our actual isolation is the explicit, freshly created
// directories passed alongside it.
function installExtensions(vscodeExecutablePath: string, extensionsDirectory: string, userDataDirectory: string, vsixPaths: readonly string[]): void {
    const [cli, ...cliArgs] = resolveCliArgsFromVSCodeExecutablePath(vscodeExecutablePath, { reuseMachineInstall: true });
    const args = [...cliArgs, `--extensions-dir=${extensionsDirectory}`, `--user-data-dir=${userDataDirectory}`, ...ROOT_ARGS];
    for (const vsixPath of vsixPaths) {
        args.push('--install-extension', vsixPath);
    }
    const result = spawnSync(cli, args, { encoding: 'utf-8', stdio: 'inherit', shell: process.platform === 'win32' });
    if (result.status !== 0) {
        throw new Error(`Installing extensions into ${extensionsDirectory} failed with exit code ${String(result.status)}.`);
    }
}

// Electron refuses to start as root without --no-sandbox, and a clean-machine container runs as root.
const ROOT_ARGS: readonly string[] = process.platform === 'linux' && typeof process.getuid === 'function' && process.getuid() === 0
    ? ['--no-sandbox']
    : [];

interface RunOptions {
    label: string;
    workspace: string;
    userDataDirectory: string;
    cacheDirectory: string;
    extensionTestsPath: string;
    extensionTestsEnv?: Record<string, string>;
}

function runDevPathMode(options: RunOptions): Promise<number> {
    return runTests({
        version: process.env.VSCODE_TEST_VERSION ?? 'stable',
        extensionDevelopmentPath: EXTENSION_ROOT,
        extensionTestsPath: options.extensionTestsPath,
        launchArgs: [
            options.workspace,
            `--user-data-dir=${options.userDataDirectory}`,
            '--disable-extensions',
            '--disable-workspace-trust',
            '--skip-welcome',
            '--skip-release-notes',
            '--disable-telemetry',
            ...ROOT_ARGS,
        ],
        extensionTestsEnv: {
            MCPPLS_TEST: '1',
            MCPPLS_E2E_WORKSPACE: options.workspace,
            MCPPLS_CACHE_DIR: options.cacheDirectory,
            ...options.extensionTestsEnv,
        },
    });
}

async function runVsixMode(options: RunOptions & { vsixPath: string; extraVsixPaths: readonly string[] }): Promise<number> {
    const vscodeExecutablePath = await downloadAndUnzipVSCode({ version: process.env.VSCODE_TEST_VERSION ?? 'stable' });
    const extensionsDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-ext-'));
    installExtensions(vscodeExecutablePath, extensionsDirectory, options.userDataDirectory, [options.vsixPath, ...options.extraVsixPaths]);

    return runTests({
        vscodeExecutablePath,
        // A minimal, code-free extension: the real extension under test is
        // the one just installed for real above, not something loaded from
        // source. See test/harness/package.json.
        extensionDevelopmentPath: path.join(EXTENSION_ROOT, 'test', 'harness'),
        extensionTestsPath: options.extensionTestsPath,
        launchArgs: [
            options.workspace,
            `--extensions-dir=${extensionsDirectory}`,
            `--user-data-dir=${options.userDataDirectory}`,
            // Deliberately no --disable-extensions: that would disable the
            // extension under test too. Isolation comes from the fresh
            // --extensions-dir and --user-data-dir above instead.
            '--disable-workspace-trust',
            '--skip-welcome',
            '--skip-release-notes',
            '--disable-telemetry',
            ...ROOT_ARGS,
        ],
        extensionTestsEnv: {
            MCPPLS_TEST: '1',
            MCPPLS_E2E_WORKSPACE: options.workspace,
            MCPPLS_CACHE_DIR: options.cacheDirectory,
            ...options.extensionTestsEnv,
        },
    });
}

async function runOnce(config: {
    label: string;
    vsixPath: string | undefined;
    extraVsixPaths: readonly string[];
    extensionTestsPath: string;
    allowedNewFiles: readonly string[];
    extensionTestsEnv?: Record<string, string>;
}): Promise<void> {
    const workspace = prepareWorkspace();
    const beforeHashes = hashWorkspace(workspace);
    const cacheDirectory = process.env.MCPPLS_CACHE_DIR ?? fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-e2e-cache-'));
    // A short user data directory of its own. VS Code listens on a socket
    // inside it, and macOS limits a socket path to 104 bytes: the default
    // under .vscode-test in a CI checkout is longer and fails with `listen
    // EINVAL`.
    const userDataDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-ud-'));
    console.log(`[${config.label}] workspace: ${workspace}`);
    console.log(`[${config.label}] server cache: ${cacheDirectory}`);
    console.log(`[${config.label}] user data: ${userDataDirectory}`);

    const options: RunOptions = {
        label: config.label,
        workspace,
        userDataDirectory,
        cacheDirectory,
        extensionTestsPath: config.extensionTestsPath,
        extensionTestsEnv: config.extensionTestsEnv,
    };
    const exitCode = config.vsixPath
        ? await runVsixMode({ ...options, vsixPath: config.vsixPath, extraVsixPaths: config.extraVsixPaths })
        : await runDevPathMode(options);
    if (exitCode !== 0) {
        throw new Error(`[${config.label}] the end-to-end suite exited with ${exitCode}.`);
    }
    assertWorkspaceUnchanged(workspace, beforeHashes, config.allowedNewFiles);
    console.log(`[${config.label}] workspace unchanged: OK`);
}

async function main(): Promise<void> {
    const scenario = process.env.MCPPLS_E2E_SCENARIO ?? 'main';
    const vsixPath = process.env.MCPPLS_E2E_VSIX;

    if (scenario === 'conflicts') {
        if (!vsixPath) {
            throw new Error(
                'MCPPLS_E2E_SCENARIO=conflicts installs the cpptools/clangd stub extensions alongside the packaged '
                + 'extension, which needs MCPPLS_E2E_VSIX=<path to the mcppls .vsix> (build one with '
                + '`npx --no-install vsce package --out <path>`).',
            );
        }
        const extensionTestsPath = path.resolve(__dirname, 'suite-conflicts', 'index');
        const stubVsixPaths = [
            packageStub(path.join(EXTENSION_ROOT, 'test', 'stubs', 'cpptools')),
            packageStub(path.join(EXTENSION_ROOT, 'test', 'stubs', 'clangd')),
        ];
        // Two full, independent runs -- see the header comment on
        // test/suite-conflicts/conflicts.test.ts for why this cannot be one
        // run with two substituted answers.
        for (const [label, answer] of [['conflicts: disable', DISABLE], ['conflicts: keep both', KEEP]] as const) {
            await runOnce({
                label,
                vsixPath,
                extraVsixPaths: stubVsixPaths,
                extensionTestsPath,
                allowedNewFiles: ['.vscode/settings.json'],
                extensionTestsEnv: { MCPPLS_E2E_CONFLICT_ANSWER: answer },
            });
        }
        return;
    }

    if (scenario === 'stress') {
        // Seeded random use (real-project plan RP0): test/suite-stress, on the same workspace.
        await runOnce({
            label: vsixPath ? 'stress (vsix)' : 'stress (development path)',
            vsixPath,
            extraVsixPaths: [],
            extensionTestsPath: path.resolve(__dirname, 'suite-stress', 'index'),
            allowedNewFiles: [],
        });
        return;
    }

    await runOnce({
        label: vsixPath ? 'main (vsix)' : 'main (development path)',
        vsixPath,
        extraVsixPaths: [],
        extensionTestsPath: path.resolve(__dirname, 'suite', 'index'),
        allowedNewFiles: [],
    });
}

main().catch((error: unknown) => {
    console.error(error instanceof Error ? error.stack ?? error.message : error);
    process.exit(1);
});
