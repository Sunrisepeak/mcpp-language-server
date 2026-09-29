// Handle construction shared by the interfaces whose handles are owned.
//
// The specification requires that a released handle not be treated as valid,
// and recommends dividing the word into an index and a generation. That is what
// this file does. It is not a translation table: the descriptor is recovered
// arithmetically from the word, and the array holds only generations, so the
// implementation retains the property clause 7.1 requires.
#pragma once
#include "sys.h"

namespace okl {

inline constexpr int kMaxDescriptor = 65536;

inline unsigned* generations() {
    static unsigned g[kMaxDescriptor];
    return g;
}

inline okl_uptr pack(int fd) {
    if (fd < 0 || fd >= kMaxDescriptor) return 0;
    return (static_cast<okl_uptr>(generations()[fd]) << 32)
         | (static_cast<okl_uptr>(fd) + 1u);
}

// Returns the descriptor, or -1 if the word does not name a live one.
//
// THIS ACCEPTS A WORD THAT WAS NEVER PACKED, AND SILENTLY. A bare descriptor N
// has the shape of a packed handle naming N-1 whose generation is still zero,
// so this returns N-1 for it rather than -1. Nothing here can tell the two
// apart: the word is one machine word and carries no tag.
//
// The consequence is that a handle of the OTHER discipline must never reach
// this function. openkal.stream's handles are bare descriptors (stream.cpp
// states why), and src/timeout.cpp used to pass one here and wait upon the
// descriptor below the one it then transferred upon. Owned handles --- kal_file,
// kal_dir, kal_net_listener, kal_net_conn, kal_datagram --- are the whole of
// this function's domain.
inline int unpack(okl_uptr h) {
    const int fd = static_cast<int>(h & 0xffffffffu) - 1;
    if (fd < 0 || fd >= kMaxDescriptor) return -1;
    if (static_cast<unsigned>(h >> 32) != generations()[fd]) return -1;
    return fd;
}

inline void retire(okl_uptr h) {
    const int fd = unpack(h);
    if (fd >= 0) ++generations()[fd];
}

}  // namespace okl
