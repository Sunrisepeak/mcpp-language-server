import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { resolveLaunch, SUPPORTED_PLATFORMS } from '../../src/payload';

suite('maintained distribution scope', () => {
    test('0.0.12 declares three maintained targets', () => {
        assert.deepStrictEqual([...SUPPORTED_PLATFORMS].sort(), ['darwin-arm64', 'linux-x64', 'win32-x64']);
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
                assert.strictEqual(external.launch.source, 'environment');
            }
        } finally {
            Object.defineProperty(process, 'platform', platform);
            fs.rmSync(directory, { recursive: true, force: true });
        }
    });
});
