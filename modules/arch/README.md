# arch

Two packages, `x86_64` and `aarch64`, each exporting the same module, `mcppls.arch`: `Arch` and
`ARCH`. Only one is ever in a build's dependency graph — the os packages pick it with
`[target.'cfg(arch = "...")'.dependencies]` — so, like `mcppls.os`, it is a compile-time constant
read with `if constexpr`, never a macro. Its one consumer is `mcppls.os`'s `PLATFORM`, the name a
payload, a VSIX and the lock use for "this operating system on this architecture".
