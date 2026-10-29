import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { currentPlatform, payloadPlatforms, platformPart, readManifest, resolveLaunch, SUPPORTED_PLATFORMS } from '../../src/payload';

suite('distribution scope', () => {
    test('every platform of the payload lock is bundled', () => {
        assert.deepStrictEqual([...SUPPORTED_PLATFORMS].sort(), ['darwin-arm64', 'linux-arm64', 'linux-x64', 'win32-x64']);
    });

    test('an external server remains usable on an unbundled host', () => {
        const platform = Object.getOwnPropertyDescriptor(process, 'platform')!;
        const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-payload-scope-'));
        const server = path.join(directory, 'server');
        fs.writeFileSync(server, 'external server fixture', { mode: 0o755 });
        try {
            Object.defineProperty(process, 'platform', { value: 'freebsd', configurable: true });
            const missing = resolveLaunch(directory, {});
            assert.strictEqual(missing.ok, false);
            const external = resolveLaunch(directory, { MCPPLS_SERVER: server });
            assert.strictEqual(external.ok, true);
            if (external.ok) {
                assert.strictEqual(external.launch.executable, server);
                assert.deepStrictEqual(external.launch.leading, []);
                assert.strictEqual(external.launch.source, 'environment');
            }
        } finally {
            Object.defineProperty(process, 'platform', platform);
            fs.rmSync(directory, { recursive: true, force: true });
        }
    });

    // The payload's server is one okb of every architecture; the runner of this platform starts it.
    test('an image server starts through the runner payload.json names', () => {
        const payload = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-payload-image-'));
        const write = (relative: string, text: string) => {
            fs.mkdirSync(path.dirname(path.join(payload, relative)), { recursive: true });
            fs.writeFileSync(path.join(payload, relative), text, { mode: 0o755 });
        };
        const manifest = (server: object) => write('payload.json', JSON.stringify({
            'payload-version': 4, platform: currentPlatform(), server,
            clangd: { version: '23', path: 'clangd/bin/clangd' }, kit: { name: 'kit', path: 'kit' },
        }));
        try {
            write('bin/mcppls.okb', 'okb');
            write('bin/okrun', 'runner');
            manifest({ version: '0.0.18', path: 'bin/mcppls.okb', runner: 'bin/okrun' });
            const image = resolveLaunch(payload, { MCPPLS_PAYLOAD: payload });
            assert.strictEqual(image.ok, true);
            if (image.ok) {
                assert.strictEqual(image.launch.executable, path.join(payload, 'bin', 'okrun'));
                assert.deepStrictEqual(image.launch.leading, [path.join(payload, 'bin', 'mcppls.okb')]);
                assert.strictEqual(image.launch.payloadDir, payload);
            }
            fs.rmSync(path.join(payload, 'bin', 'okrun'));
            const noRunner = resolveLaunch(payload, { MCPPLS_PAYLOAD: payload });
            assert.strictEqual(noRunner.ok, false);
            manifest({ version: '0.0.18', path: 'bin/mcppls.okb', runner: '../okrun' });
            const outside = resolveLaunch(payload, { MCPPLS_PAYLOAD: payload });
            assert.strictEqual(outside.ok, false, 'a runner outside the payload is not started');
            write('bin/mcppls', 'program');
            manifest({ version: '0.0.18', path: 'bin/mcppls' });
            const program = resolveLaunch(payload, { MCPPLS_PAYLOAD: payload });
            assert.strictEqual(program.ok, true);
            if (program.ok) {
                assert.strictEqual(program.launch.executable, path.join(payload, 'bin', 'mcppls'));
                assert.deepStrictEqual(program.launch.leading, []);
            }
        } finally {
            fs.rmSync(payload, { recursive: true, force: true });
        }
    });

    // One payload for every platform (payload version 5): this machine's entry names the runner and the engine.
    test('one payload starts the server through this platform\'s runner and refuses a platform it lacks', () => {
        const payload = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-payload-one-'));
        const write = (relative: string, text: string) => {
            fs.mkdirSync(path.dirname(path.join(payload, relative)), { recursive: true });
            fs.writeFileSync(path.join(payload, relative), text, { mode: 0o755 });
        };
        const entry = (system: string, platform: string) => ({
            runner: `libexec/okrun/${system}/okrun${platform.startsWith('win32') ? '.exe' : ''}`,
            engines: { clangd: { version: '23.1.0-mcppls.5', path: `clangd/${platform}/bin/clangd${platform.startsWith('win32') ? '.exe' : ''}` } },
        });
        const here = currentPlatform();
        const other = here === 'win32-x64' ? 'linux-x64' : 'win32-x64';
        const manifest = (platforms: Record<string, object>) => write('payload.json', JSON.stringify({
            'payload-version': 5, server: { version: '0.0.20', path: 'bin/mcppls.okb' }, kit: { name: 'mcppls-kit-openkal', path: 'kit' }, platforms,
        }));
        try {
            write('bin/mcppls.okb', 'okb');
            const own = entry('this-system', here);
            write(own.runner, 'runner');
            manifest({ [here]: own, [other]: entry('other-system', other) });
            const read = readManifest(payload);
            assert.ok(read.manifest, read.problem);
            assert.deepStrictEqual(payloadPlatforms(read.manifest!).sort(), [here, other].sort());
            assert.strictEqual(platformPart(read.manifest!, here)?.clangd, own.engines.clangd.path);
            const launch = resolveLaunch(payload, { MCPPLS_PAYLOAD: payload });
            assert.strictEqual(launch.ok, true);
            if (launch.ok) {
                assert.strictEqual(launch.launch.executable, path.join(payload, own.runner));
                assert.deepStrictEqual(launch.launch.leading, [path.join(payload, 'bin', 'mcppls.okb')]);
            }
            manifest({ [other]: entry('other-system', other) });
            const refused = resolveLaunch(payload, { MCPPLS_PAYLOAD: payload });
            assert.strictEqual(refused.ok, false);
            if (!refused.ok) {
                assert.ok(refused.reason.includes(here) && refused.reason.includes(other), refused.reason);
            }
            const server = path.join(payload, 'external-server');
            write('external-server', 'server');
            assert.strictEqual(resolveLaunch(payload, { MCPPLS_PAYLOAD: payload, MCPPLS_SERVER: server }).ok, true, 'MCPPLS_SERVER still names a server');
            manifest({});
            assert.ok(readManifest(payload).problem, 'a version 5 payload names its platforms');
        } finally {
            fs.rmSync(payload, { recursive: true, force: true });
        }
    });
});
