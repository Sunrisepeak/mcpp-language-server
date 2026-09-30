// Program startup, for a program that carries no runtime of its own.
//
// Something must receive control from the kernel, find the arguments the kernel
// left on the stack, establish the thread pointer, and call the program. Where
// a program already carries a runtime, that runtime's first object does it and
// this file is not reached: the definition below is used only when the linker
// has an undefined `_start' to satisfy, which happens exactly when no other
// object provides one.
//
// It belongs to the implementation rather than to whatever sits above, and the
// reason is visible in what it does. Every step is a fact about this kernel ---
// the layout of the stack at inception, the program headers the kernel reports,
// the instruction that sets the thread pointer. A consumer that contained these
// steps would contain a copy of them per environment, which is what depending
// on openkal was meant to remove.
//
// Control is handed on through `__libc_start_main', which is the name the
// arrangement already has a name for. The symbol is weak: a program written
// directly against openkal has none, and then this file runs the initialisers
// and calls `main' itself.
#ifdef OKL_STANDALONE

#include "sys.h"
#include "tls.h"
#include <openkal/memory.h>
#include <openkal/abort.h>

namespace okl {
void record(int argc, char** argv, char** envp);
okl_ulong auxval(okl_ulong key);
}

extern "C" {

int main(int, char**, char**);

[[gnu::weak]] int __libc_start_main(int (*)(int, char**, char**), int, char**,
                                    void (*)(), void (*)(), void (*)());

[[noreturn]] void __okl_start_c(long* sp);

}

namespace {

// The initialiser arrays the linker builds. Each is weak: a program with no
// initialisers has neither symbol, and the loop then runs zero times rather
// than failing to link.
using initialiser = void (*)(int, char**, char**);
extern "C" {
[[gnu::weak]] extern initialiser __preinit_array_start[];
[[gnu::weak]] extern initialiser __preinit_array_end[];
[[gnu::weak]] extern initialiser __init_array_start[];
[[gnu::weak]] extern initialiser __init_array_end[];
}

void run_initialisers(int argc, char** argv, char** envp) {
    for (initialiser* p = __preinit_array_start; p != __preinit_array_end; ++p) (*p)(argc, argv, envp);
    for (initialiser* p = __init_array_start;    p != __init_array_end;    ++p) (*p)(argc, argv, envp);
}

void* allocate(okl_uptr n, okl_uptr a) { return kal_alloc(n, a); }

}  // namespace

#if defined(__x86_64__)
__asm__(
".text\n"
".globl _start\n"
".type _start,@function\n"
"_start:\n"
"  xor %ebp,%ebp\n"        // the outermost frame has no caller
"  mov %rsp,%rdi\n"        // the kernel left everything here
"  and $-16,%rsp\n"
"  call __okl_start_c\n"
"  hlt\n"
".size _start,.-_start\n"
);
#elif defined(__aarch64__)
__asm__(
".text\n"
".globl _start\n"
".type _start,%function\n"
"_start:\n"
"  mov x29,#0\n"
"  mov x30,#0\n"
"  mov x0,sp\n"
"  and x1,x0,#-16\n"
"  mov sp,x1\n"
"  bl __okl_start_c\n"
"  brk #0\n"
".size _start,.-_start\n"
);
#endif

extern "C" [[noreturn]] void __okl_start_c(long* sp) {
    const int argc = static_cast<int>(sp[0]);
    char** argv = reinterpret_cast<char**>(sp + 1);
    char** envp = argv + argc + 1;
    okl::record(argc, argv, envp);

    // The thread pointer must exist before anything that has thread-local
    // state runs, which includes the C library's own initialisation.
    okl::describe_tls(okl::auxval(3 /* AT_PHDR */),
                      okl::auxval(4 /* AT_PHENT */),
                      okl::auxval(5 /* AT_PHNUM */));
    const okl::tls_block b = okl::make_tls(allocate);
    if (b.tp != nullptr) okl::set_thread_pointer(b.tp);

    if (__libc_start_main != nullptr) {
        __libc_start_main(main, argc, argv, nullptr, nullptr, nullptr);
        // A C library's hand-over does not return. Reaching here means one
        // did, and continuing would run the program a second time.
        kal_exit(127);
    }

    run_initialisers(argc, argv, envp);
    kal_exit(main(argc, argv, envp));
}

#endif  // OKL_STANDALONE
