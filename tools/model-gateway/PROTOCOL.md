# mcppls-model gateway protocol

`mcppls-model` is a child process. mcppls starts it and speaks this line
protocol with it over the child's standard input and output; the language
server itself never talks to the network (see README.md "Build form" and the
overall design's section 11). This document is the wire contract; any
conforming implementation of it can stand in for the reference gateway in
this repository.

## Framing

One JSON object per line, on both stdin (requests, notifications) and stdout
(responses). No other framing (no `Content-Length`, no multi-line values).

- **Request:** `{"id": <integer>, "method": "...", "params": {...}}`
- **Response (success):** `{"id": <same>, "result": ...}`
- **Response (failure):** `{"id": <same>, "error": {"code": <int>, "message": "..."}}`
- **Notification:** the same shape as a request, minus `id`; never answered.

Requests may be pipelined (sent before earlier ones are answered); responses
are not required to arrive in the order the requests were sent, though the
reference gateway happens to answer them in order since it processes one at a
time.

Logs go to stderr only; stdout carries only protocol lines. Nothing written
to either stream ever includes an API key.

## Methods

### `initialize` (request)

`params`: none required; any object sent is ignored.

`result`:

```json
{
  "protocol": 1,
  "gateway": { "name": "mcppls-model", "version": "0.1.0" },
  "models": [
    { "id": "gpt-4o-mini", "provider": "openai", "structuredOutput": true }
  ]
}
```

`models` reflects this process's own configuration (below): one entry for
the configured `--model`/`MCPPLS_MODEL_NAME`, or an empty array when no model
is configured. `structuredOutput` is `true` for `provider: "openai"` (native
`response_format` support) and `false` for `"anthropic"` (no such field here;
`complete` falls back to an instruction — see below).

### `complete` (request)

`params`:

```json
{
  "model": "gpt-4o-mini",
  "messages": [
    { "role": "system", "content": "..." },
    { "role": "user", "content": "..." }
  ],
  "schema": { "type": "object", "required": ["answer"], "properties": { "answer": { "type": "string" } } },
  "maxTokens": 512,
  "temperature": 0
}
```

- `model`: optional; defaults to `initialize`'s first (only) model. A missing
  model with none configured either answers `1002`.
- `messages`: required, non-empty; `role` is `system`, `user` or `assistant`;
  `content` is plain text (no multimodal parts, no tool calls).
- `schema`: optional JSON Schema object requesting structured output.
- `maxTokens`: optional positive integer.
- `temperature`: optional number, default `0`.

`result`:

```json
{
  "content": "...",
  "json": { "answer": "..." },
  "model": "gpt-4o-mini",
  "finishReason": "stop",
  "usage": { "inputTokens": 123, "outputTokens": 45 }
}
```

`json` is present only when `schema` was given. `finishReason` is `"stop"`
or `"length"`.

**Structured output:** when `schema` is given, the gateway asks a provider
that supports it (OpenAI: `response_format` with `type: "json_schema"`,
carrying the schema) to produce it directly; a provider without that support
(Anthropic, here) instead gets an added instruction ("respond with only a
single JSON object matching this JSON Schema ..."). Either way, the gateway
then parses the reply's text as JSON — unwrapping a single ` ```json ` fence
first, since a model sometimes adds one despite the instruction — and checks
it against the schema's top-level `required` keys and top-level `properties`
`type`s (not full JSON Schema: no nested validation, no `$ref`, no
`additionalProperties`, and so on). A reply that is not such JSON answers
error `1004`.

### `cancel` (notification)

`params`: `{"id": <request id>}`.

The pending request with that id answers error `1003` as soon as possible.
For a request against an `http://` endpoint (the common case: a local
server, and this package's own tests) that is genuinely prompt — the
gateway polls for cancellation in ~100ms slices while blocked in I/O. For an
`https://` endpoint it is best effort: `mcpplibs::tinyhttps::HttpClient`, used
for that transport, offers no way to interrupt a request already in flight,
so cancellation is only checked immediately before and after the blocking
call (see README.md "Build form").

A `cancel` for an id that is not pending (never existed, or already
answered) does nothing.

### `shutdown` (request)

`params`: none. `result`: `null`. The process keeps running after answering
this — it exits only at end of input or on an `exit` notification, per
below. Use it to signal "no more requests are coming" before closing stdin.

### `exit` (notification)

`params`: none. The process exits (code 0) as soon as this is read, without
waiting for anything already in flight.

### End of input

Closing stdin (without an `exit` notification) is also a clean shutdown: the
gateway exits once it has read everything already sent.

## Error codes

| Code | Meaning |
|---|---|
| -32700 | Parse error: the line was not valid JSON |
| -32601 | Unknown method |
| -32602 | Invalid params (missing/malformed fields) |
| 1001 | Provider error: an HTTP failure or an error response from the endpoint; `message` carries the status and, when the body parses, the provider's own message |
| 1002 | Not configured: no provider, or no model, is configured for this request |
| 1003 | Cancelled |
| 1004 | The model's reply did not parse as JSON matching the requested schema |

A parse error (`-32700`) cannot be correlated to a request id (the line
never parsed far enough to find one), so its response carries `"id": null`.

## Configuration

Command-line flags, each with an environment fallback (flag wins when both
are given):

| Flag | Environment | Meaning |
|---|---|---|
| `--provider openai\|anthropic` | `MCPPLS_MODEL_PROVIDER` | Which wire dialect and auth headers to use |
| `--endpoint <base URL>` | `MCPPLS_MODEL_ENDPOINT` | e.g. `http://127.0.0.1:11434/v1` for a local OpenAI-compatible server. Defaults to the provider's public API (`https://api.openai.com/v1`, `https://api.anthropic.com/v1`) when omitted |
| `--model <name>` | `MCPPLS_MODEL_NAME` | Default model id; a `complete` call may override it per request |
| `--api-key-env <VAR>` | `MCPPLS_MODEL_API_KEY_ENV` | The key is read from the *named* environment variable, not passed directly; absent is fine for local endpoints that need none |
| `--timeout <seconds>` | `MCPPLS_MODEL_TIMEOUT` | Per-request network timeout; default `60` |

An invalid `--provider` value, or no provider configured at all, is not a
startup error: the process still starts and answers `initialize` (with an
empty `models` list if no model is configured either); only a `complete`
call that then has no usable provider or model answers `1002`.

## Transport

- `provider: "openai"` → `POST {endpoint}/chat/completions`, `Authorization: Bearer <key>` when a key is configured.
- `provider: "anthropic"` → `POST {endpoint}/messages`, `x-api-key: <key>` and `anthropic-version: 2023-06-01` when a key is configured.

`https://` endpoints go through `mcpplibs::tinyhttps::HttpClient`. `http://`
endpoints — the local-server case this protocol is built around — go through
a small HTTP/1.1 client of the gateway's own, built on
`mcpplibs::tinyhttps::Socket` (tinyhttps's own raw-TCP class): every version
of `mcpplibs::tinyhttps` checked while building this gateway (0.2.3, 0.2.8,
0.3.0) refuses any URL scheme but `https` before opening a socket, which
cannot reach a plain-HTTP local endpoint at all. See README.md "Build form"
for the full story and why this is not a protocol-breaking deviation: it
changes nothing about the bytes on the wire to the *provider*, only which
code inside the gateway sends them.
