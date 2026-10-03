// The parts of "Reset This Workspace's Cache" that need no VS Code.
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import { advertisesCacheReset, freedText, offersCacheReset, parseCacheResetResult, RESET_CACHE_COMMAND, SERVER_RESET_CACHE_COMMAND, sizeText } from '../../src/cacheReset';
import { SERVER_SWEEP_CACHE_COMMAND } from '../../src/cacheSweep';

suite('cache reset', () => {
    test('is offered for the three issue codes and nothing else', () => {
        for (const code of ['preparation-stalled', 'module-lock-stale', 'engine-crash-loop']) {
            assert.strictEqual(offersCacheReset([{ code: 'other' }, { code }]), true, code);
        }
        assert.strictEqual(offersCacheReset([{ code: 'engine-timeout' }]), false);
        assert.strictEqual(offersCacheReset([]), false);
        assert.strictEqual(offersCacheReset(undefined), false);
    });

    test('only a server that lists the command can do it', () => {
        assert.strictEqual(advertisesCacheReset({ executeCommandProvider: { commands: ['mcppls.restartEngine', 'mcppls.resetCache'] } }), true);
        assert.strictEqual(advertisesCacheReset({ executeCommandProvider: { commands: ['mcppls.restartEngine'] } }), false);
        assert.strictEqual(advertisesCacheReset({ executeCommandProvider: {} }), false);
        assert.strictEqual(advertisesCacheReset({}), false);
        assert.strictEqual(advertisesCacheReset(undefined), false);
    });

    test('reads the result the server sends, and a malformed one as nothing freed', () => {
        assert.deepStrictEqual(parseCacheResetResult({ ok: true, freedBytes: 2048 }), { ok: true, freedBytes: 2048 });
        assert.deepStrictEqual(parseCacheResetResult({ ok: true }), { ok: true, freedBytes: 0 });
        assert.deepStrictEqual(parseCacheResetResult({ ok: true, freedBytes: 'many' }), { ok: true, freedBytes: 0 });
        assert.deepStrictEqual(parseCacheResetResult(null), { ok: false, freedBytes: 0 });
    });

    test('says how much was freed', () => {
        assert.strictEqual(sizeText(10), '1 KB');
        assert.strictEqual(sizeText(5 * 1024 * 1024), '5.0 MB');
        assert.strictEqual(sizeText(3 * 1024 * 1024 * 1024), '3.0 GB');
        assert.match(freedText({ ok: true, freedBytes: 5 * 1024 * 1024 }), /5\.0 MB freed/);
        assert.match(freedText({ ok: true, freedBytes: 0 }), /no cache to reset/);
    });
});

// vscode-languageclient registers every command the server advertises as a VS Code command; one the
// extension registered first makes the client fail with "command '...' already exists".
suite('command ids', () => {
    // What the server lists in executeCommandProvider.commands (src/engine and src/orchestrator).
    const SERVER_COMMANDS = ['mcppls.review.run', 'mcppls.review.clear', 'mcppls.reloadBuildDescription', 'mcppls.describeOnline',
        'mcppls.restartEngine', 'mcppls.exportBundle', SERVER_RESET_CACHE_COMMAND, SERVER_SWEEP_CACHE_COMMAND];
    // out/test/unit -> the extension root
    const root = path.resolve(__dirname, '..', '..', '..');

    function registeredByExtension(): string[] {
        const ids = new Set<string>([RESET_CACHE_COMMAND]);
        const sources = path.join(root, 'src');
        for (const name of fs.readdirSync(sources).filter((file) => file.endsWith('.ts'))) {
            const text = fs.readFileSync(path.join(sources, name), 'utf8');
            for (const match of text.matchAll(/registerCommand\(\s*'([^']+)'/g)) ids.add(match[1]);
            for (const match of text.matchAll(/INSTALL_COMMAND_ID\s*=\s*'([^']+)'/g)) ids.add(match[1]);
            // 0.0.10: the cache commands are held as constants (cacheSweep.ts). They are what the
            // extension registers; the SERVER_* constants elsewhere name what it *sends*.
            for (const match of text.matchAll(
                /\b(OPEN_CACHE_HUB_COMMAND|SWEEP_WORKSPACE_CACHE_COMMAND|COPY_AGENT_PROMPT_COMMAND|REVEAL_CACHE_DIRECTORY_COMMAND)\s*=\s*'([^']+)'/g,
            )) {
                ids.add(match[2]);
            }
        }
        return [...ids];
    }

    test('no command the extension registers is one the server advertises', () => {
        const registered = registeredByExtension();
        assert.ok(registered.length >= 10, `found only ${registered.join(', ')}`);
        assert.deepStrictEqual(registered.filter((id) => SERVER_COMMANDS.includes(id)), []);
    });

    test('the palette command and the server command are different ids', () => {
        assert.notStrictEqual(RESET_CACHE_COMMAND, SERVER_RESET_CACHE_COMMAND);
    });

    test('every contributed command is registered by the extension or advertised by the server', () => {
        const manifest = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8')) as { contributes: { commands: { command: string }[] } };
        const known = new Set([...registeredByExtension(), ...SERVER_COMMANDS]);
        assert.deepStrictEqual(manifest.contributes.commands.map((entry) => entry.command).filter((id) => !known.has(id)), []);
    });
});
