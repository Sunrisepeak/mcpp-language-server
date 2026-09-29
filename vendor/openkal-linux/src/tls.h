// Thread-local storage for a program that has no C library to establish it.
//
// openkal reports, through kal_task_props, whether a context started by
// kal_task_start observes the thread-local storage of the toolchain that
// compiled the program. This implementation reports that it does, and this
// file is what makes the report true when the program is linked without a C
// library: the register that names the current context's storage is set by
// whoever creates the context, and in that configuration that is this
// implementation.
//
// The layout is the processor's, not the specification's. It is described by
// the psABI of each architecture and is reproduced here rather than obtained
// from a C library, for the reason src/sys.h gives.
#pragma once
#include "sys.h"

namespace okl {

// The program's own thread-local segment, as the loader described it.
struct tls_image {
    const unsigned char* data;   // the initialised part
    okl_uptr filesz;
    okl_uptr memsz;
    okl_uptr align;
    bool     known;
};

inline tls_image& image() { static tls_image i{}; return i; }

inline okl_uptr round_up(okl_uptr n, okl_uptr to) { return (n + to - 1) & ~(to - 1); }

// Reads the program's own headers, which the kernel reports at inception. The
// loader has already applied any bias, so the addresses are the ones the
// program will use.
inline void describe_tls(okl_ulong phdr, okl_ulong phent, okl_ulong phnum) {
    struct elf_phdr {
        okl_u32 type, flags;
        okl_u64 offset, vaddr, paddr, filesz, memsz, align;
    };
    constexpr okl_u32 pt_tls = 7;
    auto& im = image();
    im.known = true;
    if (phdr == 0 || phnum == 0) return;
    for (okl_ulong i = 0; i < phnum; ++i) {
        const auto* p = reinterpret_cast<const elf_phdr*>(phdr + i * phent);
        if (p->type != pt_tls) continue;
        im.data   = reinterpret_cast<const unsigned char*>(p->vaddr);
        im.filesz = static_cast<okl_uptr>(p->filesz);
        im.memsz  = static_cast<okl_uptr>(p->memsz);
        im.align  = p->align < 16 ? 16 : static_cast<okl_uptr>(p->align);
        return;
    }
}

// How large a region one context's storage occupies, and where within it the
// thread pointer goes. Two conventions exist and both are represented, because
// the two architectures this implementation supports use one each.
struct tls_block { void* base; okl_uptr bytes; okl_uptr align; void* tp; };

inline tls_block make_tls(void* (*alloc)(okl_uptr, okl_uptr)) {
    const auto& im = image();
    const okl_uptr align = im.align ? im.align : 16;
    tls_block b{};
    b.align = align;

#if defined(__x86_64__)
    // Variant II: the storage lies below the thread pointer, and the word the
    // thread pointer addresses holds the thread pointer itself, which is how a
    // program obtains it without an instruction that reads the register.
    const okl_uptr size = round_up(im.memsz, align);
    b.bytes = size + 64;
    b.base  = alloc(b.bytes, align);
    if (!b.base) return b;
    auto* base = static_cast<unsigned char*>(b.base);
    fill(base, 0, b.bytes);
    // The linker measured every offset backwards from the thread pointer, so
    // the initialised image sits at the start of the region and the thread
    // pointer at its end.
    if (im.data && im.filesz) copy(base, im.data, im.filesz);
    b.tp = base + size;
    // The word the thread pointer addresses holds the thread pointer itself.
    // A program obtains it with one load and no instruction that reads the
    // segment register, which is why the convention exists.
    *reinterpret_cast<void**>(b.tp) = b.tp;
#elif defined(__aarch64__)
    // Variant I: the storage lies above the thread pointer, after a gap of two
    // words reserved by the procedure call standard.
    const okl_uptr gap = round_up(16, align);
    b.bytes = gap + round_up(im.memsz, align) + 64;
    b.base  = alloc(b.bytes, align);
    if (!b.base) return b;
    auto* base = static_cast<unsigned char*>(b.base);
    fill(base, 0, b.bytes);
    if (im.data && im.filesz) copy(base + gap, im.data, im.filesz);
    b.tp = base;
#endif
    return b;
}

inline void set_thread_pointer(void* tp) {
#if defined(__x86_64__)
    sys(nr_arch_prctl, 0x1002 /* ARCH_SET_FS */, reinterpret_cast<okl_long>(tp));
#elif defined(__aarch64__)
    __asm__ __volatile__("msr tpidr_el0, %0" :: "r"(tp));
#endif
}

}  // namespace okl
