// The series rule (docs/92-release.md "Versions"): a version is X.Y.Z, X.Y the series the server
// and every plugin share, Z each component's own patch. A plugin works with any server of its
// series and refuses one of another, naming both versions and the install that matches.

// The X.Y of a three-part version; undefined for any other text.
export function seriesOf(version: string): string | undefined {
    const m = /^(\d+)\.(\d+)\.(\d+)(?:[-+].*)?$/.exec(version.trim());
    return m ? `${m[1]}.${m[2]}` : undefined;
}

// Why this extension cannot use the server, or undefined when it can. A server that states no
// version, or a version of another shape, is not refused: nothing is known against it.
export function seriesMismatch(extensionVersion: string, serverVersion: string | undefined): string | undefined {
    if (!serverVersion) return undefined;
    const ours = seriesOf(extensionVersion);
    const theirs = seriesOf(serverVersion);
    if (!ours || !theirs || ours === theirs) return undefined;
    return `The language server is mcppls ${serverVersion} (series ${theirs}); this extension, C++ Modules ${extensionVersion}, ` +
        `works with series ${ours}. Install the extension again to restore the server it carries, or name a server ` +
        `of series ${ours} with MCPPLS_SERVER (xlings install mcppls@${ours}).`;
}
