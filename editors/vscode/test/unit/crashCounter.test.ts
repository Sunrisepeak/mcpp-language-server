// Server crashes against the restart budget (normal stops are never counted), and the crash report folder.
import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { CrashCounter, CRASH_WINDOW_MS, MAX_CRASHES } from '../../src/crashCounter';
import { newestServerLog, tailLines, writeCrashReport } from '../../src/crashReport';

suite('crash counter', () => {
    test('gives up on the third crash inside the window', () => {
        const counter = new CrashCounter();
        assert.deepStrictEqual(counter.record(0, false), { count: 1, giveUp: false });
        assert.deepStrictEqual(counter.record(1000, false), { count: 2, giveUp: false });
        assert.deepStrictEqual(counter.record(2000, false), { count: MAX_CRASHES, giveUp: true });
    });

    test('a stop the extension asked for is never counted', () => {
        const counter = new CrashCounter();
        for (let i = 0; i < 10; i += 1) {
            assert.deepStrictEqual(counter.record(i, true), { count: 0, giveUp: false });
        }
        assert.deepStrictEqual(counter.record(100, false), { count: 1, giveUp: false });
    });

    test('crashes older than the window are forgotten, and a restart clears them', () => {
        const counter = new CrashCounter();
        counter.record(0, false);
        counter.record(1000, false);
        assert.strictEqual(counter.record(CRASH_WINDOW_MS + 500, false).count, 2);
        counter.reset();
        assert.strictEqual(counter.record(CRASH_WINDOW_MS + 1600, false).count, 1);
    });
});

suite('crash report', () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'mcppls-crash-test-'));
    suiteTeardown(() => fs.rmSync(directory, { recursive: true, force: true }));

    test('keeps the last lines only', () => {
        const text = Array.from({ length: 700 }, (_, i) => `line ${i}`).join('\n') + '\n';
        const tail = tailLines(text, 500).split('\n');
        assert.strictEqual(tail.length, 500);
        assert.strictEqual(tail[0], 'line 200');
        assert.strictEqual(tail[499], 'line 699');
    });

    test('takes the newest log beside the one the report named', () => {
        const logs = path.join(directory, 'logs');
        fs.mkdirSync(logs);
        const old = path.join(logs, 'a.log');
        const fresh = path.join(logs, 'b.log');
        fs.writeFileSync(old, 'old');
        fs.writeFileSync(fresh, 'fresh');
        fs.utimesSync(old, new Date(2020, 1, 1), new Date(2020, 1, 1));
        assert.strictEqual(newestServerLog(old), fresh);
        assert.strictEqual(newestServerLog(fresh), fresh);
        assert.strictEqual(newestServerLog(undefined), undefined);
        assert.strictEqual(newestServerLog(path.join(directory, 'nowhere', 'x.log')), undefined);
    });

    test('writes the folder, redacted', () => {
        const home = path.join(directory, 'home', 'someone');
        const log = path.join(directory, 'server.log');
        fs.writeFileSync(log, Array.from({ length: 600 }, (_, i) => `s${i} ${home}/x`).join('\n'));
        const written = writeCrashReport({
            storageDirectory: directory,
            now: new Date('2026-09-30T10:00:00Z'),
            crashes: 3,
            clientLog: [`client ${home}`],
            serverStderr: ['boom'],
            serverLogFile: log,
            versions: { version: '0.0.7', platform: 'linux-x64' },
            who: { home, user: 'someone' },
        });
        assert.deepStrictEqual([...written.files].sort(), ['client.log', 'info.json', 'server-log-tail.txt', 'server-stderr.txt']);
        const tail = fs.readFileSync(path.join(written.directory, 'server-log-tail.txt'), 'utf8');
        assert.strictEqual(tail.trim().split('\n').length, 500);
        assert.ok(!tail.includes(home) && tail.includes('~/x'));
        const info = JSON.parse(fs.readFileSync(path.join(written.directory, 'info.json'), 'utf8')) as Record<string, string>;
        assert.strictEqual(info.version, '0.0.7');
        assert.strictEqual(info.time, '2026-09-30T10:00:00.000Z');
    });

    test('still writes when no server log is known', () => {
        const written = writeCrashReport({
            storageDirectory: directory, now: new Date('2026-09-30T11:00:00Z'), crashes: 3, clientLog: [], serverStderr: [],
            serverLogFile: undefined, versions: {}, who: { home: '/nowhere-home', user: '' },
        });
        assert.ok(!written.files.includes('server-log-tail.txt'));
        assert.ok(fs.readFileSync(path.join(written.directory, 'info.json'), 'utf8').includes('no server log file was known'));
    });
});
