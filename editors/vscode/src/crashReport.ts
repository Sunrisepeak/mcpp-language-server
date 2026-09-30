// The small folder the extension writes when the server process keeps dying and the client stops
// restarting it (0.0.7 plan): the client's log, the tail of the server's newest log, versions,
// platform and time. Written under the extension's global storage, redacted like the rest of a
// report, never uploaded. Imports nothing from VS Code, so plain-Node tests load it.

import * as fs from 'fs';
import * as path from 'path';
import { redactText, Who } from './redact';

export const SERVER_LOG_TAIL_LINES = 500;

export function tailLines(text: string, count = SERVER_LOG_TAIL_LINES): string {
    const lines = text.split(/\r?\n/);
    if (lines.length > 0 && lines[lines.length - 1] === '') lines.pop();
    return lines.slice(-count).join('\n');
}

// The server's log: the file its report named, or, when that file is gone, the newest regular file
// beside it (a new session writes a new file). Undefined when nothing is known.
export function newestServerLog(knownFile: string | undefined): string | undefined {
    if (!knownFile) return undefined;
    try {
        const directory = path.dirname(knownFile);
        const candidates = fs.readdirSync(directory, { withFileTypes: true })
            .filter((entry) => entry.isFile())
            .map((entry) => {
                const full = path.join(directory, entry.name);
                return { full, time: fs.statSync(full).mtimeMs };
            })
            .sort((a, b) => b.time - a.time);
        if (candidates.length === 0) return fs.existsSync(knownFile) ? knownFile : undefined;
        // The known file wins when it is as new as anything beside it: it is the one the report named.
        const known = candidates.find((candidate) => candidate.full === knownFile);
        return known && known.time >= candidates[0].time ? knownFile : candidates[0].full;
    } catch {
        return fs.existsSync(knownFile) ? knownFile : undefined;
    }
}

export interface CrashReportInput {
    // The extension's global storage directory; the report goes in <dir>/crash-reports/<stamp>.
    storageDirectory: string;
    now: Date;
    crashes: number;
    clientLog: readonly string[];
    // What the server wrote to stderr, latest lines (the client sees it even when no log file is known).
    serverStderr: readonly string[];
    serverLogFile: string | undefined;
    versions: Record<string, unknown>;
    who: Who;
}

export interface CrashReportWritten {
    directory: string;
    files: string[];
}

function stamp(now: Date): string {
    return now.toISOString().replace(/[:.]/g, '-');
}

export function writeCrashReport(input: CrashReportInput): CrashReportWritten {
    const directory = path.join(input.storageDirectory, 'crash-reports', stamp(input.now));
    fs.mkdirSync(directory, { recursive: true });
    const files: string[] = [];
    const write = (name: string, content: string): void => {
        fs.writeFileSync(path.join(directory, name), redactText(content, input.who), 'utf8');
        files.push(name);
    };
    write('client.log', `${input.clientLog.join('\n')}\n`);
    write('server-stderr.txt', `${input.serverStderr.join('\n')}\n`);
    const logFile = newestServerLog(input.serverLogFile);
    let logNote = 'no server log file was known';
    if (logFile) {
        try {
            write('server-log-tail.txt', `${tailLines(fs.readFileSync(logFile, 'utf8'))}\n`);
            logNote = `the last ${SERVER_LOG_TAIL_LINES} lines of ${logFile}`;
        } catch (error) {
            logNote = `${logFile} could not be read: ${error instanceof Error ? error.message : String(error)}`;
        }
    }
    write('info.json', `${JSON.stringify({
        time: input.now.toISOString(),
        reason: `the server process closed unexpectedly ${input.crashes} times and was not restarted`,
        serverLog: logNote,
        ...input.versions,
    }, null, 2)}\n`);
    return { directory, files };
}
