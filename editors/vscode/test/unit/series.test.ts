// The series rule: a server of the extension's series is accepted, one of another refused with
// both versions and the install that matches (docs/92-release.md "Versions").
import * as assert from 'assert';
import { seriesMismatch, seriesOf } from '../../src/series';

suite('series', () => {
    test('the series of a three-part version', () => {
        assert.strictEqual(seriesOf('0.21.3'), '0.21');
        assert.strictEqual(seriesOf('1.2.0-rc1'), '1.2');
        assert.strictEqual(seriesOf('2026.11.8.1'), undefined);
        assert.strictEqual(seriesOf('dev'), undefined);
    });
    test('a server of the same series is used, whatever its patch', () => {
        assert.strictEqual(seriesMismatch('0.21.1', '0.21.0'), undefined);
        assert.strictEqual(seriesMismatch('0.21.0', '0.21.4'), undefined);
    });
    test('a server of another series is refused, naming both and the install', () => {
        const why = seriesMismatch('0.21.1', '0.22.0');
        assert.ok(why);
        assert.ok(why!.includes('0.22.0') && why!.includes('0.21.1'));
        assert.ok(why!.includes('xlings install mcppls@0.21'));
    });
    test('a server that states no version, or another shape, is not refused', () => {
        assert.strictEqual(seriesMismatch('0.21.0', undefined), undefined);
        assert.strictEqual(seriesMismatch('0.21.0', 'dev-build'), undefined);
    });
});
