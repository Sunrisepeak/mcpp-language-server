import { AbstractMessageWriter, Disposable, Message, MessageWriter } from 'vscode-jsonrpc/node';

// WA-VSCODE-004: vscode-jsonrpc 9.0.2 throws from an async Promise executor
// after rejecting a failed request write, leaving a second rejection unowned.
// A failed write invalidates the protocol stream. Report connection closure
// instead: the connection disposes and rejects its pending requests normally.
export class TransportWriter extends AbstractMessageWriter implements MessageWriter {
    private closed = false;
    private readonly listeners: Disposable[];

    constructor(private readonly writer: MessageWriter) {
        super();
        this.listeners = [
            writer.onError(([error, message, count]) => this.fireError(error, message, count)),
            writer.onClose(() => this.close()),
        ];
    }

    private close(): void {
        if (this.closed) return;
        this.closed = true;
        // End stdin as well: a server whose output is still open must receive
        // EOF rather than survive the failed connection as an orphan process.
        try {
            this.writer.end();
        } catch (error) {
            this.fireError(error);
        }
        this.fireClose();
    }

    async write(message: Message): Promise<void> {
        if (this.closed) return;
        try {
            await this.writer.write(message);
        } catch (error) {
            this.fireError(error, message);
            this.close();
        }
    }

    end(): void { this.writer.end(); }

    override dispose(): void {
        for (const listener of this.listeners) listener.dispose();
        this.writer.dispose();
        super.dispose();
    }
}
