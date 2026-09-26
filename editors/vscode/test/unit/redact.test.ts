// src/redact.ts in plain Node (`npm run test:unit`): the extension's own part of Collect Report names no one.
import * as assert from 'assert';
import { distinctiveName, redactJson, redactText } from '../../src/redact';

suite('the extension redacts its own part of a report', () => {
    test('the home directory is ~ with either separator, and only as a whole path', () => {
        const who = { home: '/home/alicewonder', user: 'alicewonder' };
        assert.strictEqual(redactText('/home/alicewonder/proj', who), '~/proj');
        assert.strictEqual(redactText('/home/alicewonderland/proj', who), '/home/alicewonderland/proj');
        assert.strictEqual(redactText('ran by alicewonder', who), 'ran by <user>');
    });

    test('a Windows profile is ~ in a JSON document, in either case', () => {
        const who = { home: 'C:\\Users\\runneradmin', user: 'runneradmin' };
        const report = { workspaceFolders: ['c:\\users\\RunnerAdmin\\proj', 'D:\\a\\proj'], note: 'runneradmin' };
        assert.deepStrictEqual(redactJson(report, who), { workspaceFolders: ['~\\proj', 'D:\\a\\proj'], note: '<user>' });
    });

    test('a common user name is replaced only where it names a directory', () => {
        const who = { home: '/home/runner', user: 'runner' };
        assert.strictEqual(redactText('/opt/runner/x and the test runner', who), '/opt/<user>/x and the test runner');
        assert.strictEqual(distinctiveName('runner'), false);
        assert.strictEqual(distinctiveName('bob'), false);
        assert.strictEqual(distinctiveName('runneradmin'), true);
    });
});
