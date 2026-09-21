# mcppls-model

The reference model gateway for mcppls (overall design section 7.5, work
item M2). A separate mcpp package, built and versioned independently of the
language server: mcppls never talks to the network itself (openkal, which
the server is built on, deliberately has no DNS resolution and no TLS), so
when a user opts into a model source, mcppls starts this executable as a
child process and speaks the line protocol in [PROTOCOL.md](PROTOCOL.md)
with it over stdio.

```
mcppls-model --provider openai --endpoint http://127.0.0.1:11434/v1 --model qwen2.5-coder
```

talks to a local OpenAI-compatible server (Ollama, llama.cpp, vLLM, ...);
pointed at `https://api.openai.com/v1` or `https://api.anthropic.com/v1`
(the defaults when `--endpoint` is omitted) with `--api-key-env` naming a
variable that holds a real key, it talks to the corresponding cloud API. See
PROTOCOL.md for the full method list, error codes and configuration.

## Build form

On openkal, like every other program in this repository: LLVM 22.1.8 with
`openkal-llvm-runtime` (openkal beneath musl, libc++ above it), one source for
every target, cross-built from Linux (`.agents/docs/design.md` §2.2).

The package is a workspace member, so its third-party versions come from the
root `mcpp.toml`'s `[workspace.dependencies]` and cannot drift from the
server's.

One knock-on effect: `mcpplibs::tinyhttps` (0.2.3 through at least 0.3.0,
checked directly) refuses every URL scheme but `https` before opening a
socket, including for `127.0.0.1`. That is unrelated to the openkal-vs-normal
question above — the same restriction holds in both forms — but it does mean
neither `mcpplibs::llmapi` nor `mcpplibs::tinyhttps::HttpClient` alone can
reach the plain-HTTP local endpoints PROTOCOL.md's `--endpoint` example and
this package's own tests need. See "Dependencies" below.

## Dependencies

- `nlohmann.json` 3.12.0 (`import nlohmann.json;`) for every JSON value:
  requests and responses on the wire, and the OpenAI/Anthropic request and
  response bodies.
- `mcpplibs.cmdline` 0.0.2 (`import mcpplibs.cmdline;`) for `--provider` /
  `--endpoint` / `--model` / `--api-key-env` / `--timeout`.
- `mcpplibs.tinyhttps` (the workspace's version; `import mcpplibs.tinyhttps;`), used directly
  rather than through `mcpplibs.llmapi`:
  - `tinyhttps::HttpClient` sends every `https://` request (the default
    OpenAI/Anthropic base URLs, and any `https://` endpoint a user
    configures).
  - `tinyhttps::Socket` — tinyhttps's own raw-TCP class, exported from its
    top-level module alongside `HttpClient` — is the transport for
    `http://` endpoints. `mcppls.model.transport` adds only the thin,
    deliberately minimal HTTP/1.1 request/response framing on top (always
    sends `Connection: close`, so there is no chunked-transfer decoder to
    write), and polls for cancellation in ~100ms slices while waiting on
    I/O, so a `cancel` notification reaches a request blocked reading a slow
    reply promptly (exercised by the "slow" case in `tests/`).

  `mcpplibs.llmapi` 0.2.8 was tried first, per the task, and set aside: its
  `OpenAI`/`Anthropic` provider classes call straight into
  `tinyhttps::HttpClient::send`, with no seam to redirect a request through
  anything else — so the `https`-only restriction above is not something a
  caller of llmapi can route around, only something a caller of tinyhttps's
  own lower-level pieces can. llmapi also vendors its own copy of
  `nlohmann::json` as `mcpplibs.llmapi.nlohmann.json` (a separate module from
  the top-level `nlohmann.json` this package also needs); using tinyhttps
  directly avoids depending on both.

**Known trap** (see `.agents` project memory, confirmed while writing this
package): `nlohmann::json x { otherJson }` — braced-init with one `json`
argument — selects the `initializer_list<json>` constructor and produces a
one-element *array* wrapping `otherJson`, not a copy of it. Every JSON
value built here uses `=`, not `{ }`, whenever the right-hand side is
itself a `nlohmann::json` value (an empty `Json::array()`/`Json::object()`,
or a whole request/response body assembled in one expression).

## Layout

C++23 named modules only (`.cppm` interfaces + `.cpp` implementation units,
no headers of our own, no macros — a `module;` global fragment is used only
for `<winsock2.h>`/POSIX socket headers `mcpplibs::tinyhttps::Socket` itself
needs, the same pattern tinyhttps uses). Every module is `mcppls.model.<name>`;
every one opens `namespace mcppls::model` (the src/-directory convention the
main server's modules already use — see the overall design's section 4.2).

| File | Module | Role |
|---|---|---|
| `src/protocol.cppm`/`.cpp` | `mcppls.model.protocol` | The `{id, result\|error}` envelope and error codes |
| `src/config.cppm`/`.cpp` | `mcppls.model.config` | `--flag` / `MCPPLS_MODEL_*` resolution |
| `src/schema.cppm`/`.cpp` | `mcppls.model.schema` | The `required`-keys / top-level-`type`s check for `complete`'s `schema` |
| `src/transport.cppm`/`.cpp` | `mcppls.model.transport` | One POST-JSON call, over `http://` or `https://` |
| `src/provider.cppm`/`.cpp` | `mcppls.model.provider` | OpenAI-compatible and Anthropic request building / response parsing |
| `src/gateway.cppm`/`.cpp` | `mcppls.model.gateway` | The stdio loop: reads and dispatches, threads a `cancel` through to an in-flight call |
| `src/main.cpp` | — | Entry point (not a module, like the server's own `src/main.cpp`) |

## Building

```bash
mcpp build -p model-gateway                              # this host
mcpp build -p model-gateway --target aarch64-macos       # from any host
mcpp build -p model-gateway --target x86_64-windows-gnu  # from any host
```

## Tests

`mcpp test -p model-gateway` — no real provider and no API key (PROTOCOL.md's
error paths need a provider that misbehaves on command, which no real one
offers to do). `tests/test_gateway.cpp` holds both halves:

- **A mock provider**: an OpenAI-compatible `POST /v1/chat/completions` on
  `127.0.0.1`, port chosen by the system (`mcppls.platform.net`). The reply is
  chosen by the last user message: `plain`, `json-ok`, `json-bad`, `http-500`,
  `slow` (answers after 4 s, for the cancel check).
- **The gateway under test is the test binary itself**, started again with
  `--as-gateway`: a test is compiled with every module of the package, so the
  gateway's `run` is already linked in. That keeps the check honest -- a
  separate process, the line protocol over real pipes, HTTP over a real socket
  -- without the test having to find the bin target under a fingerprint
  directory.

The checks: `initialize`; a plain completion with usage; a schema completion
returning `result.json`; error `1004` for `json-bad`; error `1001` for
`http-500`; `cancel` answering `1003` within 3 s of the notification (the mock
would take 4); `shutdown`; exit 0 at the end of input. 7 cases, 25 assertions,
on every host CI runs (`mcpp test --workspace`).
