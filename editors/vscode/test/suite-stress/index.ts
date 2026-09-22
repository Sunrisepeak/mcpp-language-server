// Separate Mocha entry point for the stress scenario (MCPPLS_E2E_SCENARIO=stress): seeded random use
// of the installed extension through VS Code's own provider commands (real-project plan RP0).

import * as path from 'path';
import Mocha from 'mocha';
import { glob } from 'glob';

export async function run(): Promise<void> {
    const mocha = new Mocha({ ui: 'tdd', color: true, timeout: 300_000 });
    const testsRoot = path.resolve(__dirname);
    const files = await glob('**/*.test.js', { cwd: testsRoot });
    for (const file of files.sort()) {
        mocha.addFile(path.resolve(testsRoot, file));
    }
    await new Promise<void>((resolve, reject) => {
        mocha.run((failures) => (failures > 0 ? reject(new Error(`${failures} test(s) failed.`)) : resolve()));
    });
}
