# Naming and ownership

One server, one short name, four editor plugins that have to be recognisable as the same thing in
four marketplaces that each spell identity differently.

## Ownership

Maintained by **Sunrisepeak** (<speakshen@163.com>) as a personal project. Copyright lines, package
`authors`, plugin vendor and publisher fields all say that and nothing else.

The repository is `Sunrisepeak/mcpp-language-server`. If the project moves under an organisation
later, the places to change are the ones listed below — and the schema `$id`s, discussed at the end.

## The names

| | Value | Why |
|---|---|---|
| Project / package | `mcpp-language-server` | What it is, spelled out; the name a package index carries. The PACKAGE only — every plugin uses the short name below |
| Executable, module namespace, settings prefix | `mcppls` | What you type and what you read in code: `mcppls serve`, `import mcppls.lsp`, `mcppls.trace` |
| Plugin display name | **C++ Modules (mcppls)** | What it does first, so it is findable; the short name in parentheses so four listings read as one product |
| Semantic kit package | `mcppls-kit` | A separate artifact with its own version, named after what produces it |

## Plugin identity per editor

Each marketplace has its own identifier shape. The rule is the same one everywhere: the owner
segment is `sunrisepeak`, the product segment is the short name.

| Editor | Identifier | Display name | Notes |
|---|---|---|---|
| VS Code | `sunrisepeak.mcppls` | C++ Modules (mcppls) | `publisher.name` from `package.json`; the publisher must exist on the Marketplace before a publish |
| Zed | `mcppls` | C++ Modules (mcppls) | Zed extension ids are flat, so the short name is the id |
| CLion / IntelliJ | `io.github.sunrisepeak.mcppls` | C++ Modules (mcppls) | JetBrains wants reverse-DNS; `io.github.<user>` is the form for a personal project |
| Claude Code | `mcppls-lsp` in marketplace `mcppls` | C++ Modules (mcppls) | A plugin inside this repository's own marketplace file |

Settings and commands are `mcppls.*` in every editor that has them, so one name answers "what is
this called" across the product.

The VS Code extension is `sunrisepeak.mcppls`, not `sunrisepeak.mcpp-language-server`. The package
and the plugin are two different things — xlings installs the server, a marketplace installs the
plugin — and of the two names only `mcppls` is the one already carried by the executable, the
settings prefix, the module namespace and `mcppls-kit`. Being findable is the display name's job,
and it says "C++ Modules" first for exactly that reason.

## Schema identifiers

The three JSON Schema `$id`s under [`specs/schema/`](specs/schema/) read
`https://github.com/mcpp-community/mcpp-language-server/...`. A schema `$id` is an identifier, not a
link: consumers key off it, and changing it is a version event for that schema. They are left as they
are until a schema's next version.
