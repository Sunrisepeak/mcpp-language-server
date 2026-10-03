// S-1, S-2 (plan 0.0.9) in plain Node: a renamed setting is read from its new name first, then from the
// old one, and the options sent to the server carry `engine.workers` under the names the server's
// registry knows.
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import {
    buildInitializationOptions, InspectedValue, legacyNamesInUse, RENAMED_SETTINGS, resolveRenamed, SettingsReader, workersSetting,
} from '../../src/settingsRead';

/** A configuration with the given user values: `global` for the user's settings, `workspace` for the folder's. */
function reader(global: Record<string, unknown> = {}, workspace: Record<string, unknown> = {}): SettingsReader {
    const inspect = (key: string): InspectedValue => ({ globalValue: global[key], workspaceValue: workspace[key] });
    return {
        get: <T>(key: string, fallback?: T): T | undefined => (workspace[key] ?? global[key] ?? fallback) as T | undefined,
        inspect,
    };
}

const ENGINE = RENAMED_SETTINGS[0];
const DISCOVERY = RENAMED_SETTINGS[1];

suite('S-1: the renamed settings', () => {
    test('a value under the new name wins over the old name, at any scope', () => {
        assert.strictEqual(resolveRenamed(reader({ 'engine.name': 'clangd', engine: 'none' }), ENGINE, 'clangd'), 'clangd');
        assert.strictEqual(resolveRenamed(reader({ engine: 'none' }, { 'engine.name': 'clangd' }), ENGINE, 'clangd'), 'clangd');
    });

    test('the old name applies when the new one has no value of its own', () => {
        assert.strictEqual(resolveRenamed(reader({ engine: 'none' }), ENGINE, 'clangd'), 'none');
        assert.strictEqual(resolveRenamed(reader({}, { buildDiscovery: 'off' }), DISCOVERY, 'auto'), 'off');
    });

    test('an old value that is not a string is ignored, and nothing at all is the default', () => {
        assert.strictEqual(resolveRenamed(reader({ engine: true }), ENGINE, 'clangd'), 'clangd');
        assert.strictEqual(resolveRenamed(reader(), DISCOVERY, 'auto'), 'auto');
    });

    test('the notice is for an old name with a value of a person\'s own, and only that', () => {
        assert.deepStrictEqual(legacyNamesInUse(reader()), []);
        assert.deepStrictEqual(legacyNamesInUse(reader({ engine: 'none' }, { buildDiscovery: 'off' })), [ENGINE, DISCOVERY]);
    });

    test('a person who set only a new sub-setting is not told about a rename', () => {
        // VS Code reads `engine` back as the parent object of `engine.workers` and `engine.name`.
        assert.deepStrictEqual(legacyNamesInUse(reader({ engine: { workers: '4' } }, { buildDiscovery: { providers: ['cmake'] } })), []);
        assert.strictEqual(resolveRenamed(reader({ engine: { workers: '4' } }), ENGINE, 'clangd'), 'clangd');
        // The old string in the user's settings still applies under a workspace that set only `engine.workers`.
        assert.strictEqual(resolveRenamed(reader({ engine: 'none' }, { engine: { workers: '4' } }), ENGINE, 'clangd'), 'none');
    });
});

suite('S-2: what the server is sent', () => {
    test('engine.workers is sent, and only auto or 1 to 99 is', () => {
        assert.strictEqual(buildInitializationOptions(reader({ 'engine.workers': '4' }), '')['engine.workers'], '4');
        assert.strictEqual(buildInitializationOptions(reader(), '')['engine.workers'], 'auto');
        for (const bad of ['0', '100', 'many', '-1', '4 ']) {
            assert.strictEqual(workersSetting(bad), bad === '4 ' ? '4' : 'auto', bad);
        }
        assert.strictEqual(workersSetting(''), 'auto');
        assert.strictEqual(workersSetting(undefined), 'auto');
    });

    test('the renamed settings are sent under their new names, whichever name a person used', () => {
        const options = buildInitializationOptions(reader({ engine: 'none', buildDiscovery: 'off' }), '');
        assert.strictEqual(options['engine.name'], 'none');
        assert.strictEqual(options['buildDiscovery.mode'], 'off');
        assert.ok(!('engine' in options) && !('buildDiscovery' in options), 'the old names are not sent');
    });

    test('every property of package.json that the server reads is in the options', () => {
        const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, '../../../package.json'), 'utf8')) as {
            contributes: { configuration: { properties: Record<string, unknown> } };
        };
        const options = buildInitializationOptions(reader(), '');
        const sent = (key: string): boolean => key in options
            || (key.includes('.') && key.split('.')[0] in options && typeof options[key.split('.')[0]] === 'object');
        // Not sent because they act in the extension itself, or are read where they happen.
        // Not sent because they act in the extension itself, or are read where they happen.
        // 0.0.10: the cache display settings are the editor's own (the server renders no UI).
        const extensionOnly = new Set(['enable', 'trace.server', 'ai.enabled', 'detectConflicts', 'cache.showInStatusBar', 'statusBar.maxLength']);
        const missing = Object.keys(manifest.contributes.configuration.properties)
            .filter((name) => name.startsWith('mcppls.'))
            .map((name) => name.slice('mcppls.'.length))
            .filter((key) => !extensionOnly.has(key) && !sent(key));
        assert.deepStrictEqual(missing, [], 'a setting in package.json the server is never told about');
    });
});
