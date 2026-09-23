// The architecture of one target. Exactly one of the two arch packages is in the dependency graph
// of a build, chosen by the target's `cfg(arch = ...)`, so code branches on it with `if constexpr`
// instead of the preprocessor -- the same arrangement as the os packages, one axis over.
export module mcppls.arch;

import std;

export namespace mcppls::arch {

enum class Arch { x86_64, aarch64 };

inline constexpr Arch ARCH { Arch::aarch64 };

} // namespace mcppls::arch
