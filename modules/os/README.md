# os

`mcppls.os`, the system mcppls runs upon in mcppls' own names: `Family` and `family()`,
`family_name()`, `executable_suffix()`, `path_list_separator()`, `case_insensitive_paths()`,
`ARCH`, `arch_name()`, and `platform()` --- `linux-x64`, `darwin-arm64`, `win32-x64`, the name VS
Code gives an extension target and the key of a payload, a VSIX and
`packaging/payload.lock.json`'s platforms.

Every answer is [mcpplibs.os](https://github.com/mcpplibs/os)'s `host()`. Built for a system it is a
constant expression, so `if (mcppls::os::family() == mcppls::os::Family::windows)` folds and no
preprocessor branch is needed; built as an ok image (`<arch>-openkal`) it is answered when the image
is loaded, so one image follows the system it runs upon. `IS_IMAGE` says which of the two a build is.
