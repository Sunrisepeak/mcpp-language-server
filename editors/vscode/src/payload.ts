// Locates the language server and the payload it ships with.
//
// The extension carries one payload for every platform (`payload/payload.json`, version 5): the
// server, an image of every architecture (`bin/mcppls.okb`), the runner of every system that starts
// it, the engine (clangd) built for each platform and the openkal kit. The entry of this machine's
// platform says which runner starts the server and which clangd it drives. A payload of version 1
// to 4 is one platform's, its entry that platform's. MCPPLS_SERVER and MCPPLS_PAYLOAD point
// somewhere else during development and tests.

import * as fs from 'fs';
import * as path from 'path';

export interface PayloadEntry {
    version: string;
    path: string;
}

// The server: a program (`path`), or an image (`path`) that the program `runner` starts (version 4 and earlier).
export interface PayloadServer extends PayloadEntry {
    runner?: string;
}

// One platform's part of a version 5 payload.
export interface PayloadPlatform {
    runner: string;
    engines: { clangd: PayloadEntry };
}

export interface PayloadManifest {
    'payload-version': number;
    platform?: string;                                  // versions 1 to 4: the one platform
    server: PayloadServer;
    clangd?: PayloadEntry;                              // versions 1 to 4
    kit: { name: string; path: string };
    platforms?: Record<string, PayloadPlatform>;        // version 5: every platform
    // Version 2 (usable plan W9.4): size and sha256 of the files the server verifies at startup.
    files?: Record<string, { size: number; sha256: string }>;
}

// What this machine starts from a payload: the server image or program, the runner of an image, the engine.
export interface PlatformPart {
    runner?: string;
    clangd: string;
}

// 1: the first layout; 2 adds `files`, which the server itself checks; 3 adds `engines`, which the
// server reads to pick each engine's executable and matching kit; 4 binds an immutable engine
// identity; 5 is one payload for every platform, each with its runner and engine.
export const SUPPORTED_PAYLOAD_VERSIONS: readonly number[] = [1, 2, 3, 4, 5];

export interface ServerLaunch {
    // The program started: the server, or the runner of an image server.
    executable: string;
    // What comes before the server's own arguments: the image, when `executable` is its runner.
    leading: string[];
    // Passed to the server as `--payload`; absent when no payload directory exists.
    payloadDir?: string;
    manifest?: PayloadManifest;
    source: 'payload' | 'environment';
}

export type LaunchResolution =
    | { ok: true; launch: ServerLaunch }
    | { ok: false; reason: string };

// The platforms of packaging/payload.lock.json, as `${process.platform}-${process.arch}` names them;
// `mcppls-devtools check platforms` fails when the two lists differ.
export const SUPPORTED_PLATFORMS: readonly string[] = ['linux-x64', 'linux-arm64', 'win32-x64', 'darwin-arm64'];

export function currentPlatform(): string {
    return `${process.platform}-${process.arch}`;
}

function isFile(file: string): boolean {
    try {
        return fs.statSync(file).isFile();
    } catch {
        return false;
    }
}

function isDirectory(directory: string): boolean {
    try {
        return fs.statSync(directory).isDirectory();
    } catch {
        return false;
    }
}

// A relative path that stays inside its base directory.
function insideRelative(value: unknown): value is string {
    if (typeof value !== 'string' || value.length === 0 || path.isAbsolute(value)) {
        return false;
    }
    const normalized = path.normalize(value);
    return normalized !== '..' && !normalized.startsWith(`..${path.sep}`);
}

function wellFormedPlatforms(platforms: unknown): boolean {
    if (typeof platforms !== 'object' || platforms === null || Object.keys(platforms).length === 0) {
        return false;
    }
    return Object.values(platforms as Record<string, Partial<PayloadPlatform>>).every((entry) =>
        typeof entry === 'object' && entry !== null && insideRelative(entry.runner)
        && typeof entry.engines === 'object' && entry.engines !== null && insideRelative(entry.engines.clangd?.path));
}

export function readManifest(payloadDir: string): { manifest?: PayloadManifest; problem?: string } {
    const file = path.join(payloadDir, 'payload.json');
    let parsed: unknown;
    try {
        parsed = JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch (error) {
        return { problem: `The payload manifest ${file} cannot be read: ${error instanceof Error ? error.message : String(error)}` };
    }
    const manifest = parsed as Partial<PayloadManifest>;
    const version = manifest['payload-version'];
    if (typeof version !== 'number' || !SUPPORTED_PAYLOAD_VERSIONS.includes(version)) {
        return { problem: `The payload manifest ${file} has an unsupported payload-version.` };
    }
    const server = manifest.server;
    const common = server !== undefined && insideRelative(server.path)
        && (server.runner === undefined || insideRelative(server.runner))
        && manifest.kit !== undefined && insideRelative(manifest.kit.path);
    const perPlatform = version >= 5
        ? wellFormedPlatforms(manifest.platforms)
        : typeof manifest.platform === 'string' && manifest.clangd !== undefined && insideRelative(manifest.clangd.path);
    if (!common || !perPlatform) {
        return { problem: `The payload manifest ${file} is incomplete.` };
    }
    return { manifest: manifest as PayloadManifest };
}

// The platforms a payload carries a server and an engine for.
export function payloadPlatforms(manifest: PayloadManifest): string[] {
    if (manifest.platforms) {
        return Object.keys(manifest.platforms);
    }
    return manifest.platform ? [manifest.platform] : [];
}

// This platform's part of the payload: its entry in a version 5 payload, or a version 1 to 4 payload
// built for it. Undefined when the payload carries nothing for `platform`.
export function platformPart(manifest: PayloadManifest, platform: string): PlatformPart | undefined {
    if (manifest.platforms) {
        const entry = manifest.platforms[platform];
        return entry ? { runner: entry.runner, clangd: entry.engines.clangd.path } : undefined;
    }
    if (manifest.platform !== platform || !manifest.clangd) {
        return undefined;
    }
    return { runner: manifest.server.runner, clangd: manifest.clangd.path };
}

// Packages lose the executable bit in some installation paths; restore it when possible.
function ensureExecutable(file: string): void {
    if (process.platform === 'win32') {
        return;
    }
    try {
        fs.accessSync(file, fs.constants.X_OK);
    } catch {
        try {
            fs.chmodSync(file, 0o755);
        } catch {
            // A read-only installation keeps its modes; spawning reports the problem.
        }
    }
}

export function resolveLaunch(extensionPath: string, env: NodeJS.ProcessEnv = process.env): LaunchResolution {
    const payloadDir = env.MCPPLS_PAYLOAD && env.MCPPLS_PAYLOAD.length > 0
        ? path.resolve(env.MCPPLS_PAYLOAD)
        : path.join(extensionPath, 'payload');
    const hasPayload = isDirectory(payloadDir);

    let manifest: PayloadManifest | undefined;
    let part: PlatformPart | undefined;
    if (hasPayload) {
        const read = readManifest(payloadDir);
        if (read.problem) {
            return { ok: false, reason: read.problem };
        }
        manifest = read.manifest;
        part = manifest ? platformPart(manifest, currentPlatform()) : undefined;
        if (part) {
            const clangd = path.join(payloadDir, part.clangd);
            if (isFile(clangd)) {
                ensureExecutable(clangd);
            }
        }
    }

    const override = env.MCPPLS_SERVER;
    if (override && override.length > 0) {
        if (!path.isAbsolute(override) || !isFile(override)) {
            return { ok: false, reason: `MCPPLS_SERVER does not name an existing executable: ${override}` };
        }
        ensureExecutable(override);
        return { ok: true, launch: { executable: override, leading: [], payloadDir: hasPayload ? payloadDir : undefined, manifest, source: 'environment' } };
    }

    // One payload serves every platform it has an entry for; one without this platform's is refused, saying which it has.
    if (manifest && !part) {
        return {
            ok: false,
            reason: `This C++ Modules package carries no server for ${currentPlatform()}; it carries ${payloadPlatforms(manifest).join(', ') || 'none'}. Install the extension again, or name a server with MCPPLS_SERVER.`,
        };
    }
    if (!hasPayload || !manifest || !part) {
        return {
            ok: false,
            reason: SUPPORTED_PLATFORMS.includes(currentPlatform())
                ? 'The C++ Modules payload is missing from this installation. Reinstall the extension.'
                : `C++ Modules has no package for ${currentPlatform()} yet.`,
        };
    }
    const server = path.join(payloadDir, manifest.server.path);
    if (!isFile(server)) {
        return { ok: false, reason: `The language server is missing: ${server}. Reinstall the extension.` };
    }
    if (part.runner === undefined) {
        ensureExecutable(server);
        return { ok: true, launch: { executable: server, leading: [], payloadDir, manifest, source: 'payload' } };
    }
    const runner = path.join(payloadDir, part.runner);
    if (!isFile(runner)) {
        return { ok: false, reason: `The program that starts the language server is missing: ${runner}. Reinstall the extension.` };
    }
    ensureExecutable(runner);
    return { ok: true, launch: { executable: runner, leading: [server], payloadDir, manifest, source: 'payload' } };
}
