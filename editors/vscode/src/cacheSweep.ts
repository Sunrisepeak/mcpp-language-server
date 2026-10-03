// The sweep command, the way `cacheReset.ts` holds the reset one (0.0.10 plan C-13.1, D6): the
// extension's command id and the server's are different on purpose -- `vscode-languageclient`
// registers every command the server advertises, and a clash fails the client at startup. Pure: no
// `vscode`, so the parsing and the ids are unit-testable.
import { CacheDetail, sizeText } from './cacheSegment';
import { t } from './strings';

export const SWEEP_WORKSPACE_CACHE_COMMAND = 'mcppls.sweepWorkspaceCache';
export const SERVER_SWEEP_CACHE_COMMAND = 'mcppls.sweepCache';
export const OPEN_CACHE_HUB_COMMAND = 'mcppls.openCacheHub';
export const COPY_AGENT_PROMPT_COMMAND = 'mcppls.copyAgentPrompt';
export const REVEAL_CACHE_DIRECTORY_COMMAND = 'mcppls.revealCacheDirectory';

export const SWEEP_CATEGORIES = ['copies', 'instances', 'trash', 'staleCommands', 'budget'] as const;
export type SweepCategory = (typeof SWEEP_CATEGORIES)[number];

export function advertisesCacheSweep(capabilities: { executeCommandProvider?: { commands?: readonly string[] } } | undefined): boolean {
    return capabilities?.executeCommandProvider?.commands?.includes(SERVER_SWEEP_CACHE_COMMAND) === true;
}

export interface SweepResult {
    ok: boolean;
    freedBytes: number;
    files: number;
    instances: number;
    roots: number;
    dryRun: boolean;
    alreadyRunning?: boolean;
}

/** Anything the server answered that is not the shape it promised counts as "nothing freed" (like `parseCacheResetResult`). */
export function parseSweepResult(value: unknown): SweepResult {
    const object = typeof value === 'object' && value !== null ? (value as Record<string, unknown>) : {};
    const number = (key: string): number => {
        const raw = object[key];
        return typeof raw === 'number' && Number.isFinite(raw) && raw > 0 ? raw : 0;
    };
    return {
        ok: object['ok'] === true,
        freedBytes: number('freedBytes'),
        files: number('files'),
        instances: number('instances'),
        roots: number('roots'),
        dryRun: object['dryRun'] === true,
        alreadyRunning: object['alreadyRunning'] === true || undefined,
    };
}

/** What the hub and the hover card say a sweep did (C-13.3: the receipt says "no restart, no rebuild"). */
export function sweepResultText(result: SweepResult): string {
    if (result.alreadyRunning === true) return t('A sweep is already running.');
    if (result.dryRun) {
        return result.freedBytes > 0
            ? t('A sweep would free {0} ({1} files). Nothing was removed.', sizeText(result.freedBytes), result.files)
            : t('A sweep would free nothing: there is nothing to remove.');
    }
    if (result.freedBytes > 0) {
        return t('Freed {0} ({1} files). No restart, no rebuild.', sizeText(result.freedBytes), result.files);
    }
    return t('Nothing to remove: the cache is already swept.');
}

/**
 * The last `cxxModules/cache` answer, remembered for the hover card. The card refreshes on status
 * notifications with the coarse numbers; the detail (four classes, largest modules, instance list)
 * comes from the last time the hub or a sweep fetched it. Plain module state, not a service.
 */
let lastDetail: CacheDetail | undefined;
export function rememberCacheDetail(detail: CacheDetail): void {
    lastDetail = detail;
}
/** The remembered detail is void after a reset: what was true of the old cache must not ride the card into the new one. */
export function forgetCacheDetail(): void {
    lastDetail = undefined;
}
export function cachedCacheDetail(): CacheDetail | undefined {
    return lastDetail;
}
