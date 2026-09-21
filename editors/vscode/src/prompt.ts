// The two questions this extension ever asks a user (another C++ extension
// conflicts with it; the macOS SDK is missing) share one test-mode
// substitution mechanism, because they share the same problem in tests: a
// real vscode.window.showInformationMessage() in the automated test host is
// never clicked by anyone, so awaiting it would hang the suite forever, and
// even if something resolved it, going through the same vscode.window object
// the zero-notification counter wraps (see installUiCounters in
// extension.ts) would make a deliberate, once-only question indistinguishable
// from an unwanted notification.
//
// So in test mode the question never reaches the real API. Instead:
//   - every call is still recorded, so a test can assert "shown once" even
//     though nothing appeared on screen (see promptShownCount in TestApi);
//   - the answer comes from whatever the test substituted via
//     TestApi.setPromptAnswer(kind, answer). That lookup is asynchronous and
//     tolerates either order of events: the extension's own automatic check
//     can run before a test has any chance to call setPromptAnswer (VS Code
//     can activate the extension as soon as it opens the workspace, which
//     may be before the test file's own setup runs), so a prompt with no
//     answer yet waits for one instead of reading a stale "not substituted"
//     value;
//   - if nothing is ever substituted, the question resolves as dismissed
//     after a short grace period rather than hanging forever -- the safe
//     default for a suite that never expects to see it. Each of the two
//     questions is only ever raised when its precondition genuinely holds
//     (a conflicting extension actually installed; the SDK actually
//     missing), which none of the ordinary suites arrange, so this default
//     is not expected to be exercised outside a test that forgets to
//     substitute an answer.

import * as vscode from 'vscode';

export type PromptKind = 'conflict' | 'commandLineTools';

const TEST_MODE = process.env.MCPPLS_TEST === '1';
const SUBSTITUTION_GRACE_MS = 5000;

class Deferred<T> {
    resolve!: (value: T) => void;
    readonly promise: Promise<T>;
    constructor() {
        this.promise = new Promise<T>((resolve) => {
            this.resolve = resolve;
        });
    }
}

class PromptTestHarness {
    private readonly answers = new Map<PromptKind, string | undefined>();
    private readonly waiting = new Map<PromptKind, Deferred<string | undefined>[]>();
    private readonly shown = new Map<PromptKind, number>();

    setAnswer(kind: PromptKind, answer: string | undefined): void {
        this.answers.set(kind, answer);
        const waiters = this.waiting.get(kind);
        if (waiters && waiters.length > 0) {
            this.waiting.set(kind, []);
            for (const waiter of waiters) {
                waiter.resolve(answer);
            }
        }
    }

    shownCount(kind: PromptKind): number {
        return this.shown.get(kind) ?? 0;
    }

    ask(kind: PromptKind): Promise<string | undefined> {
        this.shown.set(kind, this.shownCount(kind) + 1);
        if (this.answers.has(kind)) {
            return Promise.resolve(this.answers.get(kind));
        }
        const deferred = new Deferred<string | undefined>();
        const waiters = this.waiting.get(kind) ?? [];
        waiters.push(deferred);
        this.waiting.set(kind, waiters);
        const timeout = new Promise<string | undefined>((resolve) => {
            setTimeout(() => resolve(undefined), SUBSTITUTION_GRACE_MS);
        });
        return Promise.race([deferred.promise, timeout]);
    }
}

// Exported so extension.ts can wire TestApi.setPromptAnswer/promptShownCount
// straight through without another layer of indirection.
export const promptTestHarness = TEST_MODE ? new PromptTestHarness() : undefined;

// message/items are only used outside test mode; in test mode nothing is
// ever put on screen, see the module comment above.
export function askOnce(kind: PromptKind, message: string, ...items: string[]): Thenable<string | undefined> {
    if (promptTestHarness) {
        return promptTestHarness.ask(kind);
    }
    return vscode.window.showInformationMessage(message, ...items);
}
