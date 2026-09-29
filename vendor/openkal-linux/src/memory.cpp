#include "sys.h"
#include <openkal/memory.h>

// Clause 7.3 requires that where the environment already provides an
// allocator, this one be built upon it and not beside it. The environment of
// this implementation is the kernel, and the kernel provides mappings rather
// than an allocator, so an allocator is what this file supplies.
//
// The hazard the clause names is nevertheless worth checking against rather
// than dismissing. It is two independent claimants upon one region, on a
// system whose heap grows by extending a single region --- that is, `brk'. No
// allocation here uses `brk'. A program that contains both this allocator and
// a C library's therefore has two allocators drawing on disjoint mappings, and
// neither can shorten the other's.
//
// Version 0.4 called the host C library's `malloc'. That is correct for a
// program above a C library and wrong for a program that is one: the ported
// library defines `malloc' too, so the call resolves to it, and it in turn
// calls this implementation. See src/sys.h.

namespace okl { okl_ulong auxval(okl_ulong key); }

namespace {


constexpr okl_uptr kPage      = 4096;
constexpr okl_uptr kMinBlock  = 16;
constexpr okl_uptr kMaxSmall  = 32768;
constexpr okl_uptr kChunk     = 1u << 20;
constexpr int      kClasses   = 12;   // 16, 32, ... 32768

// Contention is rare --- the allocations of a C library above this one are
// served by that library's own allocator, and this one sees page-granular
// requests --- so a lock that spins is adequate and needs no suspension
// primitive, which would make openkal.memory depend upon openkal.task.
struct spin {
    volatile int held = 0;
    void lock() {
        while (__atomic_exchange_n(&held, 1, __ATOMIC_ACQUIRE)) okl::sys(okl::nr_sched_yield);
    }
    void unlock() { __atomic_store_n(&held, 0, __ATOMIC_RELEASE); }
};

spin  g_lock;
void* g_free[kClasses];

int class_of(okl_uptr n) {
    okl_uptr s = kMinBlock; int c = 0;
    while (s < n) { s <<= 1; ++c; }
    return c;
}
okl_uptr size_of(int c) { return kMinBlock << c; }

void* map(okl_uptr bytes) {
    const okl_long r = okl::sys(okl::nr_mmap, 0, static_cast<okl_long>(bytes),
                                okl::prot_read | okl::prot_write,
                                okl::map_private | okl::map_anonymous, -1, 0);
    if (okl::failed(r)) return nullptr;
    return reinterpret_cast<void*>(r);
}

void unmap(void* p, okl_uptr bytes) {
    okl::sys(okl::nr_munmap, reinterpret_cast<okl_long>(p), static_cast<okl_long>(bytes));
}

okl_uptr round_up(okl_uptr n, okl_uptr to) { return (n + to - 1) & ~(to - 1); }

// A chunk is page-aligned and a block size is a power of two no larger than a
// chunk, so a block at a multiple of its size is aligned to its size. That is
// why an alignment request is satisfied by choosing a larger class rather than
// by padding: no metadata is needed, and none is kept.
bool refill(int c) {
    const okl_uptr bs = size_of(c);
    auto* base = static_cast<unsigned char*>(map(kChunk));
    if (!base) return false;
    for (okl_uptr off = kChunk; off >= bs; off -= bs) {
        auto* b = base + off - bs;
        *reinterpret_cast<void**>(b) = g_free[c];
        g_free[c] = b;
    }
    return true;
}

}  // namespace

extern "C" {

void* kal_alloc(kal_uintptr size, kal_uintptr align) {
    if (size == 0) return nullptr;
    if (align < kMinBlock) align = kMinBlock;

    okl_uptr want = size < align ? align : size;
    if (want <= kMaxSmall) {
        const int c = class_of(want);
        g_lock.lock();
        if (!g_free[c] && !refill(c)) { g_lock.unlock(); return nullptr; }
        void* b = g_free[c];
        g_free[c] = *reinterpret_cast<void**>(b);
        g_lock.unlock();
        return b;
    }

    const okl_uptr bytes = round_up(size, kPage);
    if (align <= kPage) return map(bytes);

    // An alignment wider than a page is satisfied by mapping more and
    // recording, immediately before the region returned, what must be
    // released. The record is reachable because the returned address is at
    // least sixteen bytes above the mapping's base by construction.
    const okl_uptr total = bytes + align;
    auto* base = static_cast<unsigned char*>(map(total));
    if (!base) return nullptr;
    auto  addr = reinterpret_cast<okl_uptr>(base) + 16;
    addr = (addr + align - 1) & ~(align - 1);
    auto* user = reinterpret_cast<unsigned char*>(addr);
    reinterpret_cast<okl_uptr*>(user)[-1] = total;
    reinterpret_cast<okl_uptr*>(user)[-2] = reinterpret_cast<okl_uptr>(base);
    return user;
}

// The size and alignment are those passed to the allocation. The interface
// carries them so that an implementation need keep no record of its own, and
// this one keeps none: the class is recomputed from the size, and only the
// over-aligned case --- where the returned address is not the mapping's ---
// records anything at all.
void kal_free(void* p, kal_uintptr size, kal_uintptr align) {
    if (!p || size == 0) return;
    if (align < kMinBlock) align = kMinBlock;

    const okl_uptr want = size < align ? align : size;
    if (want <= kMaxSmall) {
        const int c = class_of(want);
        g_lock.lock();
        *reinterpret_cast<void**>(p) = g_free[c];
        g_free[c] = p;
        g_lock.unlock();
        return;
    }

    if (align <= kPage) { unmap(p, round_up(size, kPage)); return; }

    auto* user = static_cast<unsigned char*>(p);
    const okl_uptr total = reinterpret_cast<okl_uptr*>(user)[-1];
    auto*  base = reinterpret_cast<void*>(reinterpret_cast<okl_uptr*>(user)[-2]);
    unmap(base, total);
}


// The quantum this environment allocates and protects memory in.
//
// AN OPERATION AND NOT A CONSTANT, BECAUSE IT IS A PROPERTY OF THE MACHINE THE
// PROGRAM RUNS ON. A C library above this reports it as its own page size; one
// that fixed it when it was built is wrong on every machine whose quantum
// differs from the one it was built for, which is what a distributed binary
// meets --- sixteen kilobytes on one family of hardware, sixty-four on another.
//
// The kernel states it in the auxiliary vector it leaves at inception. Where
// that vector is absent --- a program whose entry did not record it --- the
// value falls back to the architecture's smallest page, which is the smallest
// quantum this kernel ever uses and is therefore never coarser than the truth.
//
// One number, and it is the coarsest that is always safe: this kernel allocates
// and protects in the same unit, so the two are the same value here. An
// implementation on a system where they differ reports the coarser.
kal_uintptr kal_memory_granularity(void) {
    const okl_ulong page = okl::auxval(6 /* AT_PAGESZ */);
    return page != 0 ? static_cast<kal_uintptr>(page) : kPage;
}

}
