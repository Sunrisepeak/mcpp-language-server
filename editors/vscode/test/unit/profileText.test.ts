import * as assert from 'assert';
import { describeProfile, describeTargetSelection, TargetSelection } from '../../src/profileText';

suite('analysis target status', () => {
    test('the adopted platform is visible alongside the compiler', () => {
        assert.strictEqual(describeProfile({ kind: 'build-toolchain', compiler: 'clang 22', stdlib: 'libc++', target: 'i686-windows-msvc' }), 'clang 22 · i686-windows-msvc');
        assert.strictEqual(describeProfile(undefined), '');
    });
    test('older profiles without compiler or platform remain readable', () => {
        assert.strictEqual(describeProfile({ kind: 'semantic-kit', stdlib: 'libc++', target: '' }), 'libc++');
    });
    test('a pending requested target does not rename the adopted profile', () => {
        const selection: TargetSelection = { mode: 'auto', requestedTarget: 'i686-windows-msvc', source: 'compile-commands', reason: 'unique explicit target', pending: true };
        assert.strictEqual(describeTargetSelection(selection), 'Target source: existing compile database · Target not applied: i686-windows-msvc · unique explicit target');
        assert.strictEqual(describeProfile({ kind: 'build-toolchain', compiler: 'clang', stdlib: '', target: 'x86_64-windows-msvc' }), 'clang · x86_64-windows-msvc');
    });
    test('applied defaults and older servers have no pending claim', () => {
        assert.strictEqual(describeTargetSelection({ mode: 'project', requestedTarget: '', source: 'project', reason: '', pending: false }), 'Target source: mcpp defaults');
        assert.strictEqual(describeTargetSelection(undefined), '');
    });
    test('an invalid setting is not described as an applied default', () => {
        const text = describeTargetSelection({ mode: 'explicit', requestedTarget: '', source: 'configuration', reason: 'invalid mcpp target setting', pending: true });
        assert.ok(text.includes('Target selection not applied'));
        assert.ok(text.includes('invalid mcpp target setting'));
        assert.ok(!text.includes('mcpp defaults'));
    });
});
