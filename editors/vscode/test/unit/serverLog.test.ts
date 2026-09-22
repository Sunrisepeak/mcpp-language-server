// src/serverLog.ts in plain Node (`npm run test:unit`): no VS Code, so CI runs it in seconds before
// the end-to-end suite, which checks the same routing through a real extension host.
import * as assert from 'assert';
import { classifyServerLine, ServerLogLevel, ServerLogRouter, ServerLogSink } from '../../src/serverLog';

class RecordingSink implements ServerLogSink {
    readonly written: [string, string][] = [];
    debug(message: string): void { this.written.push(['debug', message]); }
    info(message: string): void { this.written.push(['info', message]); }
    warn(message: string): void { this.written.push(['warn', message]); }
    error(message: string): void { this.written.push(['error', message]); }
}

suite('server stderr log levels', () => {
    test('each level the server writes is kept, and its timestamp dropped', () => {
        const levels: ServerLogLevel[] = ['debug', 'info', 'warning', 'error'];
        for (const level of levels) {
            const line = classifyServerLine(`mcppls 2026-09-22T05:30:15.843Z [${level}] the message`, undefined);
            assert.deepStrictEqual(line, { level, message: 'the message' });
        }
    });

    test('the lines of a healthy start are info and warning, not errors', () => {
        // From a real start on an mcpp project with an older mcpp.
        const sink = new RecordingSink();
        const router = new ServerLogRouter();
        for (const line of [
            'mcppls 2026-09-22T05:30:15.843Z [info] log file /home/u/.cache/mcppls/logs/server.log',
            'mcppls 2026-09-22T05:30:16.036Z [info] clangd 23.1.0 at /x/payload/clangd/bin/clangd',
            'mcppls 2026-09-22T05:30:16.454Z [info]   the kind arrived in mcpp 2026.9.15.1; upgrading gives exact module information',
            'mcppls 2026-09-22T05:30:16.783Z [warning] mcpp build --configure-only failed (2): error: unknown option: --configure-only',
        ]) {
            router.route(line, sink);
        }
        assert.deepStrictEqual(sink.written.map(([method]) => method), ['info', 'info', 'info', 'warn']);
        assert.strictEqual(sink.written[2][1], '  the kind arrived in mcpp 2026.9.15.1; upgrading gives exact module information');
        assert.strictEqual(router.count('info'), 3);
        assert.strictEqual(router.count('warning'), 1);
        assert.strictEqual(router.count('error'), 0);
    });

    test('a continuation line keeps the level of the message it continues', () => {
        const sink = new RecordingSink();
        const router = new ServerLogRouter();
        router.route('mcppls 2026-09-22T05:30:16.000Z [info] compiler output:', sink);
        router.route('  second line of the same message', sink);
        router.route('mcppls 2026-09-22T05:30:16.100Z [error] cannot write the engine database', sink);
        router.route('  and why', sink);
        assert.deepStrictEqual(sink.written, [
            ['info', 'compiler output:'],
            ['info', '  second line of the same message'],
            ['error', 'cannot write the engine database'],
            ['error', '  and why'],
        ]);
    });

    test('output that is not the log is a warning, never an error the server did not report', () => {
        const sink = new RecordingSink();
        const router = new ServerLogRouter();
        router.route("terminate called after throwing an instance of 'std::bad_alloc'", sink);
        assert.deepStrictEqual(sink.written, [['warn', "terminate called after throwing an instance of 'std::bad_alloc'"]]);
    });

    test('a restarted server does not continue the previous one', () => {
        const sink = new RecordingSink();
        const router = new ServerLogRouter();
        router.route('mcppls 2026-09-22T05:30:16.100Z [error] clangd did not answer initialize', sink);
        router.reset();
        router.route('not a log line', sink);
        assert.deepStrictEqual(sink.written[1], ['warn', 'not a log line']);
        assert.strictEqual(router.count('error'), 1, 'counts are kept across restarts');
    });
});
