// Counts unexpected closes of the server process against a budget (0.0.7 plan). A stop the
// extension asked for -- restart, turning the server off, dispose -- is never counted: the caller
// passes `expected` from the client's close handler.

export const MAX_CRASHES = 3;
export const CRASH_WINDOW_MS = 3 * 60 * 1000;

export interface CrashVerdict {
    // Crashes inside the window, this one included; 0 for a stop that was expected.
    count: number;
    // The budget is used up: stop restarting.
    giveUp: boolean;
}

export class CrashCounter {
    private times: number[] = [];

    constructor(private readonly max = MAX_CRASHES, private readonly windowMs = CRASH_WINDOW_MS) {}

    // `expected`: the extension itself stopped this server (or it is no longer the current one).
    record(now: number, expected: boolean): CrashVerdict {
        if (expected) return { count: 0, giveUp: false };
        this.times = this.times.filter((time) => now - time < this.windowMs);
        this.times.push(now);
        return { count: this.times.length, giveUp: this.times.length >= this.max };
    }

    reset(): void {
        this.times = [];
    }
}
