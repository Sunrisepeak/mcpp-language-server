import * as path from 'path';
import Mocha from 'mocha';
import { glob } from 'glob';

export async function run(): Promise<void> {
    const mocha = new Mocha({ ui: 'tdd', color: true, timeout: 300_000 });
    const testsRoot = path.resolve(__dirname);
    // MCPPLS_E2E_ONLY names the test files to run, for a machine where only some can pass: with the
    // macOS Command Line Tools hidden, only sdk-missing.test.js has an SDK-free world to test.
    const only = process.env.MCPPLS_E2E_ONLY;
    const files = await glob(only && only.length > 0 ? only : '**/*.test.js', { cwd: testsRoot });
    for (const file of files.sort()) {
        mocha.addFile(path.resolve(testsRoot, file));
    }
    await new Promise<void>((resolve, reject) => {
        mocha.run((failures) => (failures > 0 ? reject(new Error(`${failures} test(s) failed.`)) : resolve()));
    });
}
