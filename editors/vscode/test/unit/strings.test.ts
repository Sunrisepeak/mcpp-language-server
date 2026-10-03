// The localization itself (plan 2026-10-03 UI-1): the source is English, the zh-cn bundle
// translates, and the two CANNOT drift -- every string the code asks for exists in both bundles,
// every bundle entry is used, and the manifest's %keys% exist in both package.nls files.
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import { setLocalizer, t } from '../../src/strings';

// __dirname is out/test/unit; the extension root is three levels up.
const ROOT = path.resolve(__dirname, '..', '..', '..');
const EN_BUNDLE = path.join(ROOT, 'l10n', 'bundle.l10n.json');
const ZH_BUNDLE = path.join(ROOT, 'l10n', 'bundle.l10n.zh-cn.json');

function readJson(file: string): Record<string, string> {
    return JSON.parse(fs.readFileSync(file, 'utf8')) as Record<string, string>;
}

// What the sources actually ask for: every `t('...')` literal under src/. The strings the module
// owns contain no single quotes of their own, so the simple scan is exact.
function usedMessages(): Set<string> {
    const used = new Set<string>();
    const sources = path.join(ROOT, 'src');
    const walk = (directory: string): void => {
        for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
            const full = path.join(directory, entry.name);
            if (entry.isDirectory()) walk(full);
            else if (entry.name.endsWith('.ts')) {
                const text = fs.readFileSync(full, 'utf8');
                for (const match of text.matchAll(/(?:^|[^A-Za-z0-9_.])t\('([^']*)'/g)) used.add(match[1]);
            }
        }
    };
    walk(sources);
    return used;
}

suite('strings and bundles', () => {
    test('the default localizer is the source language, with {0} substitution', () => {
        assert.strictEqual(t('Ready'), 'Ready');
        assert.strictEqual(t('Preparing index {0}/{1}', 9, 20), 'Preparing index 9/20');
    });

    test('a custom localizer takes over until the next set (this is how zh renders)', () => {
        const original = t('Ready');
        setLocalizer((message, ...args) => args.length > 0 ? `[zh] ${message} ${args.join(',')}` : `[zh] ${message}`);
        assert.strictEqual(t('Ready'), '[zh] Ready');
        assert.strictEqual(t('{0} modules', 7), '[zh] {0} modules 7');
        setLocalizer((message, ...args) => args.length > 0 ? message.replace(/\{(\d+)\}/g, (_, i) => String(args[Number(i)])) : message);
        assert.strictEqual(t('Ready'), original);
    });

    test('every message the code asks for exists in BOTH bundles, and no bundle entry is unused', () => {
        const en = readJson(EN_BUNDLE);
        const zh = readJson(ZH_BUNDLE);
        const used = usedMessages();
        for (const message of used) {
            assert.ok(message in en, `missing from bundle.l10n.json: ${message}`);
            assert.ok(message in zh, `missing from bundle.l10n.zh-cn.json: ${message}`);
        }
        for (const key of Object.keys(en)) {
            assert.ok(used.has(key), `bundle entry no code asks for: ${key}`);
        }
        assert.deepStrictEqual(Object.keys(en).sort(), Object.keys(zh).sort(), 'the two bundles carry the same keys');
    });

    test('the zh bundle really translates: no value equals its English key', () => {
        const zh = readJson(ZH_BUNDLE);
        const untranslated = Object.entries(zh).filter(([key, value]) => key === value).map(([key]) => key);
        assert.deepStrictEqual(untranslated, [], `untranslated in the zh bundle: ${untranslated.join(', ')}`);
    });

    test('every %key% of package.json exists in BOTH package.nls files, with no drift between them', () => {
        const pkg = fs.readFileSync(path.join(ROOT, 'package.json'), 'utf8');
        const used = new Set([...pkg.matchAll(/%([A-Za-z0-9._-]+)%/g)].map((match) => match[1]));
        const en = readJson(path.join(ROOT, 'package.nls.json'));
        const zh = readJson(path.join(ROOT, 'package.nls.zh-cn.json'));
        for (const key of used) {
            assert.ok(key in en, `missing from package.nls.json: ${key}`);
            assert.ok(key in zh, `missing from package.nls.zh-cn.json: ${key}`);
        }
        assert.deepStrictEqual(Object.keys(en).sort(), Object.keys(zh).sort(), 'the two nls files carry the same keys');
        for (const key of Object.keys(en)) {
            assert.ok(used.has(key), `nls entry package.json does not reference: ${key}`);
        }
    });
});
