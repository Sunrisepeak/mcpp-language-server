// The extension's own part of a diagnostic report (workspace folders, versions, settings), redacted
// the way the server redacts its part (issue #23 fix plan F18, S3-5.5-3): the home directory as `~`,
// the user's name as `<user>`. The server's report and bundle go through the server's own rules;
// this covers only the few fields the extension adds itself. Imports nothing from VS Code, so the
// plain-Node unit tests can load it.

export interface Who {
    home: string;   // os.homedir()
    user: string;   // os.userInfo().username
}

// Names too short or too common to replace wherever they stand ("runner", "admin"): replaced only
// where they name a directory, the same rule as the server's.
const COMMON = new Set(['admin', 'administrator', 'user', 'users', 'guest', 'root', 'test', 'runner', 'build', 'developer', 'home', 'public',
    'default', 'shared', 'ubuntu', 'docker', 'vagrant', 'owner', 'work', 'workspace', 'code', 'server', 'client', 'data', 'temp', 'demo']);

export function distinctiveName(name: string): boolean {
    return name.length >= 4 && !COMMON.has(name.toLowerCase());
}

function escapeRegExp(text: string): string {
    return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// The home directory with either separator, doubled backslashes (JSON) and either case, as a whole
// path: "/home/speak" is not in "/home/speaker".
function homePattern(home: string): RegExp | undefined {
    const segments = home.split(/[\\/]+/).filter((segment) => segment.length > 0);
    if (segments.length === 0 || (segments.length === 1 && /^[A-Za-z]:$/.test(segments[0]))) {
        return undefined;
    }
    const drive = /^[A-Za-z]:$/.test(segments[0]);
    const body = segments.map(escapeRegExp).join('[\\\\/]+');
    const source = drive ? body : `[\\\\/]+${body}`;
    return new RegExp(`${source}(?![A-Za-z0-9_])`, 'gi');
}

export function redactText(text: string, who: Who): string {
    let out = text;
    const home = homePattern(who.home);
    if (home) {
        out = out.replace(home, '~');
    }
    if (who.user.length > 0) {
        const name = escapeRegExp(who.user);
        const pattern = distinctiveName(who.user)
            ? new RegExp(`(?<![A-Za-z0-9])${name}(?![A-Za-z0-9])`, 'gi')
            : new RegExp(`(?<=[\\\\/])${name}(?![A-Za-z0-9_])`, 'gi');
        out = out.replace(pattern, '<user>');
    }
    return out;
}

// A JSON value redacted through its text; its structure is kept, since no placeholder has a quote
// or a backslash.
export function redactJson<T>(value: T, who: Who): T {
    return JSON.parse(redactText(JSON.stringify(value), who)) as T;
}
