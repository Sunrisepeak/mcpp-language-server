// 0.0.13 W7: the hub's overview says what is running -- the versions, copied without paths, and the
// running state -- and a server the extension did not ship is called out before anything else.
import * as assert from 'assert';
import { aboutText, COPY_ABOUT_COMMAND, hubItems, runningEntries } from '../../src/cacheHub';
import { CacheDetail } from '../../src/cacheSegment';
import { CxxModulesStatus } from '../../src/status';

const status: CxxModulesStatus = {
    state: 'ready',
    project: { root: 'file:///w', source: 'mcpp', tier: 1 },
    profile: { kind: 'build-toolchain', stdlib: 'libc++ 22', target: 'x86_64-linux-gnu' },
    engine: { name: 'clangd', version: '23.1.0-mcppls.1' },
    progress: { done: 25, total: 25 },
    issues: [],
    about: {
        server: '0.0.13', platform: 'linux-x64', startedAt: '2026-10-12T10:00:00Z', extension: '0.0.13',
        editor: { name: 'Visual Studio Code', version: '1.137.0' },
        engine: { name: 'clangd', version: '23.1.0-mcppls.1', build: '23.1.0-mcppls.1', features: 9, restarts: 1 },
        kit: 'mcppls-kit-libcxx-23.1.0', mcpp: '2026.10.11.1',
    },
};

const detail = {
    state: 'ready', project: { name: 'w', source: 'mcpp' }, plan: { units: 1, modules: 1 }, bytes: 0,
    canonical: { files: 0, bytes: 0 }, copies: { files: 0, bytes: 0, oldestSeconds: 0 }, trash: { bytes: 0 },
    instances: { count: 1, bytes: 0, list: [] }, largest: [], limits: { perWorkspace: 1, total: 1, over: false },
    paths: { cacheRoot: '/c', logDirectory: '/l', bundlesDirectory: '/b' }, prompts: { agent: '', issue: '' },
} as unknown as CacheDetail;

suite('hub overview: versions and running state (0.0.13 W7)', () => {
    test('the versions line names server, engine, mcpp, extension, editor and platform, and no path', () => {
        const text = aboutText(status);
        assert.strictEqual(text, 'mcppls 0.0.13 · engine clangd 23.1.0-mcppls.1 · mcpp 2026.10.11.1 · extension 0.0.13 · Visual Studio Code 1.137.0 · linux-x64');
        assert.ok(!text.includes('/'), 'no path in what is copied');
    });

    test('the overview opens with the versions (copied on click) and the running state', () => {
        const entries = runningEntries(status, Date.parse('2026-10-12T10:12:00Z'));
        assert.strictEqual(entries.length, 2);
        assert.strictEqual(entries[0].action?.command, COPY_ABOUT_COMMAND);
        assert.deepStrictEqual(entries[0].action?.arguments, [aboutText(status)]);
        assert.strictEqual(entries[1].description, 'ready · modules 25/25 · up 12 min · restarts 1');
        const items = hubItems(detail, { canSweep: true }, undefined, status);
        assert.strictEqual(items[0].kind, 'separator');
        assert.strictEqual(items[1].kind === 'entry' && items[1].icon, '$(versions)');
    });

    test('a version mismatch comes first', () => {
        const mismatched: CxxModulesStatus = { ...status, about: { ...status.about!, server: '0.0.11', extension: '0.0.13',
            mismatch: 'the editor extension is 0.0.13 but the server it started is mcppls 0.0.11' } };
        const entries = runningEntries(mismatched);
        assert.strictEqual(entries[0].icon, '$(warning)');
        assert.ok(entries[0].description?.includes('0.0.11'));
    });

    test('a cache off a Dev Drive is said once, after the running state; on one, or unknown, nothing is said', () => {
        const off: CxxModulesStatus = { ...status, about: { ...status.about!, cacheDevDrive: false } };
        const entries = runningEntries(off);
        assert.strictEqual(entries[entries.length - 1].icon, '$(shield)');
        assert.strictEqual(entries[entries.length - 1].label, 'Cache is not on a Dev Drive');
        const on: CxxModulesStatus = { ...status, about: { ...status.about!, cacheDevDrive: true } };
        assert.ok(!runningEntries(on).some((entry) => entry.icon === '$(shield)'));
        assert.ok(!runningEntries(status).some((entry) => entry.icon === '$(shield)'));
    });

    test('an older server without `about` still shows what the status has; no status shows nothing', () => {
        const old: CxxModulesStatus = { ...status, about: undefined };
        assert.strictEqual(aboutText(old), 'mcppls · clangd 23.1.0-mcppls.1');
        assert.deepStrictEqual(runningEntries(undefined), []);
        assert.strictEqual(hubItems(detail, { canSweep: true })[1].kind === 'entry' && (hubItems(detail, { canSweep: true })[1] as { label: string }).label, 'Cache in use');
    });

    test('an mcpp project whose version is not known says so instead of printing nothing', () => {
        const unknown: CxxModulesStatus = { ...status, about: { ...status.about!, mcpp: '' } };
        assert.ok(aboutText(unknown).includes('mcpp (version unknown)'));
    });
});
