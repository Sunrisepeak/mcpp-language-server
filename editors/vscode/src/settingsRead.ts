// S-1, S-2 (plan 0.0.9): the settings the extension sends the server, and the two it renamed.
//
// `mcppls.engine` and `mcppls.buildDiscovery` were plain values and the parents of other settings
// (`engine.workers`, `buildDiscovery.providers`): VS Code drops a child's default when its parent is a
// scalar, so the settings UI showed `undefined` for them. They are `mcppls.engine.name` and
// `mcppls.buildDiscovery.mode` now, and package.json no longer contributes the old names. A person's
// own value under an old name keeps working here -- `resolveRenamed` reads it when the new name has
// none -- and `settingsMigration.ts` offers, once, to move it.
//
// Plain TypeScript (no `vscode` import), so test/unit runs it in Node.

/** The fields of `WorkspaceConfiguration.inspect()` this reads. */
export interface InspectedValue {
    defaultValue?: unknown;
    globalValue?: unknown;
    workspaceValue?: unknown;
    workspaceFolderValue?: unknown;
}

/** What of a `WorkspaceConfiguration` the functions here use, so a test can stand in for it. */
export interface SettingsReader {
    get<T>(section: string, defaultValue?: T): T | undefined;
    inspect(section: string): InspectedValue | undefined;
}

/** A setting that moved: `legacy` is the name package.json used to contribute, relative to `mcppls`. */
export interface RenamedSetting {
    readonly legacy: string;
    readonly current: string;
}

export const RENAMED_SETTINGS: readonly RenamedSetting[] = [
    { legacy: 'engine', current: 'engine.name' },
    { legacy: 'buildDiscovery', current: 'buildDiscovery.mode' },
];

/** The scopes a person can have written a value in, narrowest first, as `inspect()` names them. */
export const USER_LAYERS: readonly ('workspaceFolderValue' | 'workspaceValue' | 'globalValue')[] = [
    'workspaceFolderValue', 'workspaceValue', 'globalValue',
];

/** The value a person set themselves: the narrowest scope that has one. Not the default. */
export function explicitValue(inspected: InspectedValue | undefined): unknown {
    for (const layer of USER_LAYERS) {
        if (inspected?.[layer] !== undefined) return inspected[layer];
    }
    return undefined;
}

/**
 * The value of a renamed setting: one set under the new name wins, else one set under the old name
 * when it is a string, else `fallback`. VS Code does not validate the old name any more, so what is
 * under it can be anything a person typed.
 */
export function resolveRenamed(reader: SettingsReader, renamed: RenamedSetting, fallback: string): string {
    const current = explicitValue(reader.inspect(renamed.current));
    if (typeof current === 'string') return current;
    // The narrowest scope that holds a string under the old name: a narrower one may hold the new settings' parent object
    // (`engine.workers` set in the workspace, the old `engine` string in the user's settings).
    const inspected = reader.inspect(renamed.legacy);
    for (const layer of USER_LAYERS) {
        const legacy = inspected?.[layer];
        if (typeof legacy === 'string') return legacy;
    }
    return fallback;
}

/** The old names that still have a value of a person's own, for the one-time notice. */
/**
 * The old names that still have a value of a person's own, for the one-time notice. Only a string is an old value:
 * the old name is now the parent of the new one and its siblings, so a person who set `engine.workers` alone reads
 * back an object (`{ workers: "4" }`) under `engine`, which is no old setting and must not be moved.
 */
export function legacyNamesInUse(reader: SettingsReader): RenamedSetting[] {
    return RENAMED_SETTINGS.filter((renamed) => USER_LAYERS.some((layer) => typeof reader.inspect(renamed.legacy)?.[layer] === 'string'));
}

// build description design 4.4: a value this extension does not know must not turn the network on.
function buildToolSetting(value: string | undefined): string {
    return value === 'online' || value === 'off' ? value : 'offline';
}

/** mcppls.buildDiscovery.providers' own default (config registry, settings section 9 T1): every provider. */
export const BUILD_DISCOVERY_PROVIDERS = ['mcpp', 'cmake', 'xmake', 'meson', 'compile-commands'];

const WORKERS = /^(auto|[1-9][0-9]?)?$/;

/** `engine.workers` as the server takes it: `auto` or a whole number from 1 to 99; empty means `auto`. */
export function workersSetting(value: string | undefined): string {
    const trimmed = (value ?? '').trim();
    return trimmed.length > 0 && WORKERS.test(trimmed) ? trimmed : 'auto';
}

/**
 * The `initializationOptions` of the language client: one entry per setting the server's registry
 * marks `clientConfigurable` (test/unit/settingsRead.test.ts checks that against package.json).
 * Dotted keys for the sub-settings, not nested objects -- the server accepts both (settings section 9 T1).
 */
export function buildInitializationOptions(configuration: SettingsReader, compiler: string): Record<string, unknown> {
    const get = <T>(key: string, fallback: T): T => configuration.get<T>(key, fallback) ?? fallback;
    return {
        compiler: compiler.length > 0 ? compiler : null,
        semanticKit: configuration.get<string>('semanticKit') === 'off' ? 'off' : 'auto',
        // overall design 5.6: the core semantic engine; mcppls's own module engine always runs.
        'engine.name': resolveRenamed(configuration, RENAMED_SETTINGS[0], 'clangd') === 'none' ? 'none' : 'clangd',
        // S-2 (plan 0.0.9): how many files clangd builds at once; a change restarts the server.
        'engine.workers': workersSetting(configuration.get<string>('engine.workers')),
        // The formatting fallback without a project .clang-format; a change restarts the server.
        'format.fallbackStyle': configuration.get<string>('format.fallbackStyle') || 'auto',
        // build description design 4.4: how the user's build tool may be run.
        buildTool: buildToolSetting(configuration.get<string>('buildTool')),
        // build description design 4.3: which environment it is run in.
        toolEnvironment: configuration.get<string>('toolEnvironment') === 'editor' ? 'editor' : 'auto',
        // This extension finds other C/C++ language servers itself (mcppls.detectConflicts)
        // and offers, once, to turn their language features off. Saying so keeps the server
        // from also explaining it: a server cannot see its siblings through LSP, so it tells
        // clients that arbitrate nothing -- which is every editor but this one.
        conflictArbitration: 'client',
        // Design 2026-09-25 section 7/12: `modules` is this setting; `moduleType` is fixed true
        // because this extension always declares the custom `module` semantic token type
        // (package.json contributes.semanticTokenTypes) with a `namespace` fallback for
        // themes that do not colour it.
        semanticTokens: {
            modules: get('semanticTokens.modules', true),
            moduleType: true,
        },
        // Fix plan 2026-09-26 F9: a space after `import` opens the module list. The server
        // advertises the space as a trigger character to this client unless this is off.
        completion: {
            triggerOnSpace: get('completion.triggerOnSpace', true),
        },
        // 0.0.6 plan section 3.7 B-7: whether the project's build system is detected at all, which
        // providers may be used, and whether a needed download is ever offered.
        'buildDiscovery.mode': resolveRenamed(configuration, RENAMED_SETTINGS[1], 'auto') === 'off' ? 'off' : 'auto',
        'buildDiscovery.providers': get('buildDiscovery.providers', BUILD_DISCOVERY_PROVIDERS),
        'buildDiscovery.askBeforeDownload': get('buildDiscovery.askBeforeDownload', true),
        // 0.0.6 plan section 2.6, 9 T5: implementation units opened in the background.
        'index.primeImplementationUnits': configuration.get<string>('index.primeImplementationUnits') === 'off' ? 'off' : 'auto',
    };
}
