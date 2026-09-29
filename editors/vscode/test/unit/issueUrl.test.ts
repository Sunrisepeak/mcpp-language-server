// The prefilled bug report address, and that its field ids are the ones bug_report.yml has.
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import { buildIssueUrl, issueTitle, IssueContext, MAX_URL_LENGTH, PREFILLED_FIELD_IDS, REPOSITORY, shortMessage } from '../../src/issueUrl';

// __dirname is out/test/unit; the repository root is five levels up.
const TEMPLATE = path.resolve(__dirname, '..', '..', '..', '..', '..', '.github', 'ISSUE_TEMPLATE', 'bug_report.yml');

function templateIds(): string[] {
    return [...fs.readFileSync(TEMPLATE, 'utf8').matchAll(/^\s+id:\s*([\w-]+)\s*$/gm)].map((match) => match[1]);
}

const context: IssueContext = {
    code: 'engine-crash-loop',
    message: 'clangd crashed 3 times & was stopped: "bad" <state>',
    extensionVersion: '0.0.7',
    serverVersion: '0.0.7',
    appName: 'Visual Studio Code',
    editorVersion: '1.104.0',
    platform: 'linux',
    arch: 'x64',
    bundlePath: '/tmp/mcppls bundles/bundle 1.zip',
    bundleKind: 'bundle',
};

suite('issue url', () => {
    test('every prefilled field is an id in bug_report.yml', () => {
        const ids = templateIds();
        for (const id of [...PREFILLED_FIELD_IDS, 'bundle', 'report', 'steps', 'expected', 'build-system']) {
            assert.ok(ids.includes(id), `bug_report.yml has no field ${id}`);
        }
        const url = new URL(buildIssueUrl(context));
        for (const key of url.searchParams.keys()) {
            assert.ok(key === 'template' || key === 'title' || ids.includes(key), `${key} is not a field of the form`);
        }
    });

    test('points at the repository\'s bug report form with the title and fields', () => {
        const url = new URL(buildIssueUrl(context));
        assert.strictEqual(`${url.origin}${url.pathname}`, `${REPOSITORY}/issues/new`);
        assert.strictEqual(url.searchParams.get('template'), 'bug_report.yml');
        assert.strictEqual(url.searchParams.get('title'), '[engine-crash-loop] clangd crashed 3 times & was stopped: "bad" <state>');
        assert.strictEqual(url.searchParams.get('version'), 'extension 0.0.7, server 0.0.7');
        assert.strictEqual(url.searchParams.get('editor'), 'Visual Studio Code 1.104.0');
        assert.strictEqual(url.searchParams.get('os'), 'Linux (x64)');
        const what = url.searchParams.get('what-happened') ?? '';
        assert.ok(what.includes('/tmp/mcppls bundles/bundle 1.zip'), what);
        assert.ok(/attach/i.test(what), what);
    });

    test('encodes what would break the query', () => {
        const raw = buildIssueUrl(context);
        assert.ok(!/[ "<>]/.test(raw), raw);
        assert.ok(raw.includes('title=%5Bengine-crash-loop%5D'), raw);
        assert.ok(raw.includes('%26'), 'an ampersand in the message stays inside its value');
        assert.strictEqual(new URL(raw).searchParams.size, 2 + PREFILLED_FIELD_IDS.length);
    });

    test('a crash report and a missing bundle read differently', () => {
        const crash = new URL(buildIssueUrl({ ...context, bundleKind: 'crash-report', bundlePath: '/x/crash' }));
        assert.ok((crash.searchParams.get('what-happened') ?? '').includes('crash report'));
        const none = new URL(buildIssueUrl({ ...context, bundlePath: undefined }));
        assert.ok((none.searchParams.get('what-happened') ?? '').includes('Export Diagnostic Bundle'));
        const unknown = new URL(buildIssueUrl({ ...context, extensionVersion: undefined, serverVersion: undefined }));
        assert.strictEqual(unknown.searchParams.get('version'), 'unknown');
    });

    test('a long message is shortened and the address stays openable', () => {
        const long = 'word '.repeat(2000);
        assert.ok(issueTitle('x', long).length < 100);
        assert.ok(shortMessage(long).endsWith('…'));
        assert.ok(buildIssueUrl({ ...context, message: long }).length <= MAX_URL_LENGTH);
        const huge = buildIssueUrl({ ...context, message: '€'.repeat(5000), bundlePath: '€'.repeat(3000) });
        assert.ok(huge.length <= MAX_URL_LENGTH, String(huge.length));
    });

    test('the template keeps required fields to a minimum and blank issues off', () => {
        const text = fs.readFileSync(TEMPLATE, 'utf8');
        assert.ok((text.match(/required: true/g) ?? []).length <= 2);
        const config = fs.readFileSync(path.join(path.dirname(TEMPLATE), 'config.yml'), 'utf8');
        assert.ok(/blank_issues_enabled: false/.test(config));
        assert.ok(config.includes('/issues/24'));
    });
});
