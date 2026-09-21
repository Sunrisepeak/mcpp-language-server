// Separate Mocha entry point from test/suite/index.ts, so the conflicts
// scenario (which needs the stub cpptools/clangd extensions installed and
// answers substituted before the extension activates) does not also have to
// run -- or be slowed down by -- the full modules.test.ts suite.

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
