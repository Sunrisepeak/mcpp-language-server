# mcppls for Helix

Helix has no plugin system in a release, so mcppls reaches it as configuration:
[`languages.toml`](languages.toml) declares the server and gives it the C and C++ languages. Nothing
is built or installed from here.

## The server

Helix starts `mcppls serve` from `PATH`. The release's `mcppls-<version>-openkal.zip` or single file
provides `mcppls`; the server finds its payload (the pinned clangd and the semantic kit) as
[docs/00-install.md](../../docs/00-install.md) describes.

## Install

Merge `languages.toml` into Helix's own: `~/.config/helix/languages.toml` for every project, or
`<project>/.helix/languages.toml` for one. Then check that Helix finds the server:

```bash
hx --health cpp
```

The C and C++ entries set `language-servers = ["mcppls"]`, so clangd does not start beside mcppls,
which starts its own with a module database. A list given in `languages.toml` replaces Helix's: the
`cpp` entry repeats Helix's C++ file types (25.07) and adds the module suffixes (`.ccm`, `.cxxm`,
`.c++m`, `.mpp`, `.mxx`; `.cppm` and `.ixx` are Helix's already) and `.mcpp`, so that mcpp's
`build.mcpp` is C++. If your Helix lists other types, merge the two lists by hand.

## Beside mcpp-ide

[mcpp-ide](https://github.com/mcpp-community/mcpp-ide) serves `mcpp.toml` through the host's TOML
language and does not attach to C or C++, so the two fragments name different languages and merge
without editing each other. The division of `build.mcpp` between the two servers is mcpp-ide's
`docs/rules.md`.

## Keeping mcppls off for one project

A project's `.helix/languages.toml` gives the language its servers back:

```toml
[[language]]
name = "cpp"
language-servers = ["clangd"]
```

## What was run

Helix 25.07.1 on Linux x86_64: `hx --health cpp` lists mcppls as found, and opening `build.mcpp` and
a `.cppm` file of an mcpp project starts `mcppls serve` with the language `cpp` (Helix's log,
`hx -v`). CI does not run Helix.
