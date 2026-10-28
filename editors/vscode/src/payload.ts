// Locates the language server and the payload it ships with.
//
// A platform-specific package carries `payload/payload.json`, the server,
// clangd and the standard library kit. The server is an image of every
// architecture (`bin/mcppls.okb`) that the platform's runner (`bin/okrun`)
// starts, as payload.json's `server.runner` says, or a program of the
// platform. MCPPLS_SERVER and MCPPLS_PAYLOAD point somewhere else during
// development and tests.

import * as fs from 'fs';
import * as path from 'path';

export interface PayloadEntry {
    version: string;
    path: string;
}

// The server: a program (`path`), or an image (`path`) that the program `runner` starts.
export interface PayloadServer extends PayloadEntry {
    runner?: string;
}

export interface PayloadManifest {
    'payload-version': number;
    platform: string;
    server: PayloadServer;
    clangd: PayloadEntry;
    kit: { name: string; path: string };
    // Version 2 (usable plan W9.4): size and sha256 of the files the server verifies at startup.
    files?: Record<string, { size: number; sha256: string }>;
}

// 1: the first layout; 2 adds `files`, which the server itself checks; 3 adds `engines`, which the
// server reads to pick each engine's executable and matching kit. Version 4 binds an immutable
// engine identity, which the server verifies before using its capabilities.
export const SUPPORTED_PAYLOAD_VERSIONS: readonly number[] = [1, 2, 3, 4];

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

export function readManifest(payloadDir: string): { manifest?: PayloadManifest; problem?: string } {
    const file = path.join(payloadDir, 'payload.json');
    let parsed: unknown;
    try {
        parsed = JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch (error) {
        return { problem: `The payload manifest ${file} cannot be read: ${error instanceof Error ? error.message : String(error)}` };
    }
    const manifest = parsed as Partial<PayloadManifest>;
    if (typeof manifest['payload-version'] !== 'number' || !SUPPORTED_PAYLOAD_VERSIONS.includes(manifest['payload-version'])) {
        return { problem: `The payload manifest ${file} has an unsupported payload-version.` };
    }
    if (typeof manifest.platform !== 'string'
        || !manifest.server || !insideRelative(manifest.server.path)
        || (manifest.server.runner !== undefined && !insideRelative(manifest.server.runner))
        || !manifest.clangd || !insideRelative(manifest.clangd.path)
        || !manifest.kit || !insideRelative(manifest.kit.path)) {
        return { problem: `The payload manifest ${file} is incomplete.` };
    }
    return { manifest: manifest as PayloadManifest };
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
    if (hasPayload) {
        const read = readManifest(payloadDir);
        if (read.problem) {
            return { ok: false, reason: read.problem };
        }
        manifest = read.manifest;
        if (manifest && manifest.platform !== currentPlatform()) {
            return {
                ok: false,
                reason: `This C++ Modules package is built for ${manifest.platform}, and this machine is ${currentPlatform()}. Install the package for this platform.`,
            };
        }
        if (manifest) {
            const clangd = path.join(payloadDir, manifest.clangd.path);
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

    if (!hasPayload || !manifest) {
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
    if (manifest.server.runner === undefined) {
        ensureExecutable(server);
        return { ok: true, launch: { executable: server, leading: [], payloadDir, manifest, source: 'payload' } };
    }
    const runner = path.join(payloadDir, manifest.server.runner);
    if (!isFile(runner)) {
        return { ok: false, reason: `The program that starts the language server is missing: ${runner}. Reinstall the extension.` };
    }
    ensureExecutable(runner);
    return { ok: true, launch: { executable: runner, leading: [server], payloadDir, manifest, source: 'payload' } };
}
