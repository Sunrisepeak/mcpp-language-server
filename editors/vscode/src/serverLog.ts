// The server's stderr, line by line, at the level the server gave each line.
//
// mcppls logs to stderr as `mcppls <timestamp> [<level>] <message>` (modules/base/src/log.cpp).
// vscode-languageclient's default writes every stderr line with `error()`, so a healthy start read
// as a wall of errors. Here each line keeps its own level, and loses the timestamp the log channel
// adds again. Imports nothing from VS Code, so the plain-Node unit tests can load it.

export type ServerLogLevel = 'debug' | 'info' | 'warning' | 'error';

// The LogOutputChannel methods a line is written with.
export interface ServerLogSink {
    debug(message: string): void;
    info(message: string): void;
    warn(message: string): void;
    error(message: string): void;
}

export interface ServerLogLine {
    level: ServerLogLevel;
    message: string;
}

const LOG_LINE = /^mcppls \S+ \[(debug|info|warning|error)\] ?(.*)$/;

// One stderr line, given the level of the log line before it. A line in the server's log format has
// its own level. Any other line either continues the message above (a multi-line message is one
// `write`, split by the reader) and has its level, or, before any log line, is not the log at all
// -- runtime output such as a crash message -- and is a warning: something to look at, which the
// server itself did not call an error.
export function classifyServerLine(line: string, previous: ServerLogLevel | undefined): ServerLogLine {
    const match = LOG_LINE.exec(line);
    if (match) {
        return { level: match[1] as ServerLogLevel, message: match[2] };
    }
    return { level: previous ?? 'warning', message: line };
}

// Writes each line to `sink` at its level, and counts lines per level for the test API.
export class ServerLogRouter {
    private previous: ServerLogLevel | undefined;
    private readonly counts: Record<ServerLogLevel, number> = { debug: 0, info: 0, warning: 0, error: 0 };

    route(line: string, sink: ServerLogSink): void {
        const { level, message } = classifyServerLine(line, this.previous);
        this.previous = level;
        this.counts[level] += 1;
        switch (level) {
            case 'debug':
                sink.debug(message);
                break;
            case 'info':
                sink.info(message);
                break;
            case 'warning':
                sink.warn(message);
                break;
            case 'error':
                sink.error(message);
                break;
        }
    }

    // A restarted server's first line does not continue the last one of the server before it.
    reset(): void {
        this.previous = undefined;
    }

    count(level: ServerLogLevel): number {
        return this.counts[level];
    }
}
