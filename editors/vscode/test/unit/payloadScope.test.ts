import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { currentPlatform, resolveLaunch, SUPPORTED_PLATFORMS } from '../../src/payload';

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
});
