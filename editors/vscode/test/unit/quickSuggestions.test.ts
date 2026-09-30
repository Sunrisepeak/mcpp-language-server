// WA-VSCODE-002 (0.0.8 plan E-2, E-3) in plain Node: which layer a value comes from, and when the log says
// that the C/C++ language default overrides a person's own setting. The `inspect()` shapes are the ones
// VS Code 1.132 returned for each case (the plan, §1.4).
import * as assert from 'assert';
import { overriddenByLanguageDefault, QUICK_SUGGESTIONS, sourceOf } from '../../src/quickSuggestions';

const VSCODE_DEFAULT = { other: 'offWhenInlineCompletions', comments: 'off', strings: 'off' };
const OURS = { ...QUICK_SUGGESTIONS };

suite('WA-VSCODE-002: editor.quickSuggestions for C and C++', () => {
    test('the source is the highest layer that has a value, language-specific ones first', () => {
        assert.strictEqual(sourceOf(undefined), 'unset');
        assert.strictEqual(sourceOf({ defaultValue: VSCODE_DEFAULT }), 'default');
        assert.strictEqual(sourceOf({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS }), 'defaultLanguage');
        assert.strictEqual(sourceOf({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS, globalValue: 'off' }), 'defaultLanguage',
            'a language default outranks a user setting that names no language');
        assert.strictEqual(sourceOf({ defaultLanguageValue: OURS, globalLanguageValue: { other: 'off' } }), 'globalLanguage');
        assert.strictEqual(sourceOf({ defaultLanguageValue: OURS, globalLanguageValue: 'off', workspaceFolderLanguageValue: 'on' }), 'workspaceFolderLanguage');
    });

    test('nothing is said when nothing of theirs is overridden', () => {
        assert.strictEqual(overriddenByLanguageDefault(undefined), undefined);
        assert.strictEqual(overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS }), undefined, 'a fresh install');
        assert.strictEqual(overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS, globalLanguageValue: { other: 'off' } }),
            undefined, 'they set it for C++ itself, which wins');
        assert.strictEqual(overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS, workspaceLanguageValue: 'inline' }), undefined);
        assert.strictEqual(overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS, globalValue: { other: 'on' } }),
            undefined, 'what they set is what C++ gets anyway (the kinds they leave out are the editor default, off)');
        assert.strictEqual(overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS, globalValue: { other: true, comments: false } }),
            undefined, 'booleans inside the object mean on and off');
        assert.strictEqual(overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, globalValue: 'off' }), undefined,
            'no language default in effect: their setting is what C++ gets');
    });

    test('an override of their own setting names the layer and how to keep it', () => {
        const user = overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS, globalValue: { other: 'off', comments: 'off', strings: 'off' } });
        assert.ok(user && user.includes('user setting') && user.includes('"[cpp]"') && user.includes('"[c]"') && user.includes('WA-VSCODE-002'), user);
        const workspace = overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS, globalValue: 'off', workspaceValue: false });
        assert.ok(workspace && workspace.includes('workspace setting'), 'the workspace value is the one in effect, so it is the one named');
        assert.ok(overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS, globalValue: { comments: 'on' } }),
            'comments turned on for every language are off in C++ with the language default');
        assert.ok(overriddenByLanguageDefault({ defaultValue: VSCODE_DEFAULT, defaultLanguageValue: OURS, workspaceFolderValue: 'offWhenInlineCompletions' }));
    });
});
