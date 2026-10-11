import * as assert from 'assert';
import { PassThrough } from 'stream';
import { createMessageConnection, ErrorCodes, ResponseError, StreamMessageReader, StreamMessageWriter } from 'vscode-jsonrpc/node';
import { TransportWriter } from '../../src/transportWriter';

suite('transport write failure', () => {
    test('a failed stdio write rejects its request without an orphaned executor rejection', async () => {
        const input = new PassThrough();
        const output = new PassThrough();
        output.destroy();
        const connection = createMessageConnection(new StreamMessageReader(input), new TransportWriter(new StreamMessageWriter(output)));
        const unhandled: unknown[] = [];
        const onUnhandled = (error: unknown): void => { unhandled.push(error); };
        process.on('unhandledRejection', onUnhandled);
        connection.onClose(() => connection.dispose());
        connection.listen();
        try {
            await assert.rejects(connection.sendRequest('probe'), (error: unknown) =>
                error instanceof ResponseError && error.code === ErrorCodes.PendingResponseRejected);
            await new Promise((resolve) => setTimeout(resolve, 30));
            assert.deepStrictEqual(unhandled, []);
        } finally {
            process.removeListener('unhandledRejection', onUnhandled);
            connection.dispose();
            input.destroy();
        }
    });

    test('a live transport preserves responses and server request errors', async () => {
        const input = new PassThrough();
        const output = new PassThrough();
        const connection = createMessageConnection(new StreamMessageReader(input), new TransportWriter(new StreamMessageWriter(output)));
        const server = createMessageConnection(new StreamMessageReader(output), new StreamMessageWriter(input));
        server.onRequest('probe', () => 42);
        server.onRequest('fail', () => new ResponseError(ErrorCodes.InvalidParams, 'bad params'));
        connection.listen();
        server.listen();
        try {
            assert.strictEqual(await connection.sendRequest('probe'), 42);
            await assert.rejects(connection.sendRequest('fail'), (error: unknown) =>
                error instanceof ResponseError && error.code === ErrorCodes.InvalidParams);
        } finally {
            connection.dispose();
            server.dispose();
            input.destroy();
            output.destroy();
        }
    });
});
