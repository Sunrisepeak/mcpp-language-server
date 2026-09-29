// "Reset This Workspace's Cache" (0.0.7 plan C-1): the pieces of it that need no `vscode`, so they are
// testable in plain Node, the same reason statusText.ts and serverLog.ts are.
//
// The server contract: the initialize result lists `mcppls.resetCache` in
// `executeCommandProvider.commands`; `workspace/executeCommand` with `{ root: <folder uri> }` stops the
// engine, deletes that workspace's cache (logs are kept), plans again and restarts the engine, and
// answers `{ ok: true, freedBytes }` or an LSP error.
//
// The two ids differ on purpose. vscode-languageclient registers every command a server advertises as a
// VS Code command, so an id the extension registers itself and the server also advertises makes the
// client fail to start ("command ... already exists"). Same as mcppls.exportDiagnosticBundle, which
// sends mcppls.exportBundle, and mcppls.restartClangd, which sends mcppls.restartEngine.

// The command in the palette and the status: the extension's own.
export const RESET_CACHE_COMMAND = 'mcppls.resetWorkspaceCache';
// The command the server advertises and is asked to run.
export const SERVER_RESET_CACHE_COMMAND = 'mcppls.resetCache';

// The status issue codes for which a reset is offered next to whatever the issue offers itself: a
// preparation that does not finish, a module lock nobody holds, an engine that crashes again and again
// are the states a cache from an earlier session can cause.
export const RESET_CACHE_ISSUE_CODES: readonly string[] = ['preparation-stalled', 'module-lock-stale', 'engine-crash-loop'];

export function offersCacheReset(issues: readonly { code: string }[] | undefined): boolean {
    return (issues ?? []).some((issue) => RESET_CACHE_ISSUE_CODES.includes(issue.code));
}

// Only a server that lists the command can do it: an older one answers `unknown command`, and the
// extension does not delete a directory of the server's own.
export function advertisesCacheReset(capabilities: { executeCommandProvider?: { commands?: readonly string[] } } | undefined): boolean {
    return capabilities?.executeCommandProvider?.commands?.includes(SERVER_RESET_CACHE_COMMAND) === true;
}

export interface CacheResetResult {
    ok: boolean;
    freedBytes: number;
}

export function parseCacheResetResult(value: unknown): CacheResetResult {
    const record = typeof value === 'object' && value !== null ? value as Record<string, unknown> : {};
    const freed = record.freedBytes;
    return { ok: record.ok === true, freedBytes: typeof freed === 'number' && Number.isFinite(freed) && freed > 0 ? freed : 0 };
}

export function sizeText(bytes: number): string {
    if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`;
    if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
    return `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

export function freedText(result: CacheResetResult): string {
    return result.freedBytes > 0
        ? `C++ Modules: this workspace's cache was reset (${sizeText(result.freedBytes)} freed). Modules are prepared again from a clean state.`
        : 'C++ Modules: this workspace had no cache to reset. Modules are prepared again from a clean state.';
}
