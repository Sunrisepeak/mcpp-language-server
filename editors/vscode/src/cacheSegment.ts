// The cache as the status bar, the hover card and the hub show it (0.0.10 plan C-13.2, D12, D20).
// Pure: no `vscode` here, so the unit tests can hold every number and every character to account.
// The shapes mirror what the server puts in `cxxModules/status.cache` (coarse) and answers for
// `cxxModules/cache` (detail), per S3 4 and S3 5.7.

export interface CacheCounts {
    files: number;
    bytes: number;
}

export interface CacheInstanceInfo {
    token: string;
    version?: string;
    root?: string;
    at?: number;
    bytes: number;
    alive?: boolean;
    own?: boolean;
}

/** The coarse numbers `cxxModules/status.cache` carries (100 MB grain, S3-4-29). */
export interface CxxCacheStatus {
    bytes: number;
    limitBytes: number;
    state: 'ok' | 'near' | 'over';
    copies: CacheCounts;
    instances: { count: number; bytes: number };
    lastSweep?: { at: number; freedBytes: number; files?: number };
}

/** One root's `cxxModules/cache` report (S3 5.7). */
export interface CacheDetail {
    state: string;
    project: { name: string; source: string; level?: number; tier?: number };
    plan?: { units: number; modules: number };
    progress?: { done: number; total: number };
    bytes: number;
    canonical?: CacheCounts;
    copies: CacheCounts & { oldestSeconds?: number };
    trash?: { bytes: number };
    instances: { count: number; bytes: number; list?: CacheInstanceInfo[] };
    largest?: { module: string; bytes: number; copies: number }[];
    limits: { perWorkspace: number; total: number; over: boolean };
    lastSweep?: { at: number; freedBytes: number; files: number; failed?: number };
    paths: { cacheRoot: string; logDirectory: string };
    cli?: { cacheQuery: string; sweep: string };
    prompts?: { agent: string; issue: string };
    engines?: { name: string; version: string; role: string; state: string }[];
}

/** D12: a codicon renders as an icon, not as monospace text; two characters is the measure. */
export const ICON_WIDTH = 2;

/** The visible width of one status-bar segment: `$(name)` counts as ICON_WIDTH, every other character as 1. */
export function textWidth(text: string): number {
    let total = 0;
    let index = 0;
    while (index < text.length) {
        if (text.startsWith('$(', index)) {
            const end = text.indexOf(')', index + 2);
            if (end === -1) {
                total += text.length - index;
                break;
            }
            total += ICON_WIDTH;
            index = end + 1;
        } else {
            total += 1;
            index += 1;
        }
    }
    return total;
}

/** Bytes as a person reads them: `3.79 GB`, `0 B` (decimal, like the disk numbers people compare against). */
export function sizeText(bytes: number): string {
    if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let value = bytes;
    let unit = 0;
    while (value >= 1000 && unit < units.length - 1) {
        value /= 1000;
        unit += 1;
    }
    const digits = value >= 100 || unit === 0 ? 0 : value >= 10 ? 1 : 2;
    return `${value.toFixed(digits)} ${units[unit]}`;
}

/** The cache's own tier: what it colours the whole item with when it is the worst thing in sight. */
export type Tier = 0 | 1 | 2;

export interface CacheSegment {
    icon: string;
    text: string;
    tier: Tier;
}

/**
 * The S2 segment (D10): what the cache says, at the tier its fill level earns. `auto` visibility is
 * the caller's decision (only tiers 1 and 2 are shown then); this answers what the segment *is*.
 */
export function cacheSegment(cache: CxxCacheStatus | undefined): CacheSegment | undefined {
    if (!cache || !Number.isFinite(cache.bytes) || cache.bytes < 0) return undefined;
    const size = sizeText(cache.bytes);
    if (cache.state === 'over') {
        return { icon: '$(warning)', text: size, tier: 2 };
    }
    if (cache.state === 'near' && cache.limitBytes > 0) {
        return { icon: '$(database)', text: `${size}/${sizeText(cache.limitBytes)}`, tier: 1 };
    }
    return { icon: '$(database)', text: size, tier: 0 };
}

/** The whole item's tier is the worst of its segments (C-13.2: the cache never hides the module state, only colours with it). */
export function combineTier(moduleTier: Tier, cacheTier: Tier | undefined): Tier {
    return Math.max(moduleTier, cacheTier ?? 0) as Tier;
}

/** True when the segment fits beside the module text within the budget (D12). */
export function fits(budget: number, ...segments: string[]): boolean {
    return segments.reduce((total, segment) => total + textWidth(segment), 0) <= budget;
}

/** The settings, as numbers: the length budget in characters (D12). */
export function clampMaxLength(value: unknown): number {
    const parsed = typeof value === 'number' ? value : Number.parseInt(String(value ?? ''), 10);
    if (!Number.isFinite(parsed)) return 36;
    return Math.min(60, Math.max(24, Math.trunc(parsed)));
}
