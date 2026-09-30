#include "sys.h"
#include "tls.h"
#include <openkal/task.h>
#include <openkal/memory.h>

// Execution contexts come from one of two places, and which one is a property
// of the program rather than of this implementation.
//
// A program that already carries a runtime has that runtime's threads, and a
// context obtained from them may call anything the program can call. This is
// the ordinary arrangement and it is the default.
//
// A program that carries no such runtime --- because it supplies one itself, or
// because it has none --- has nothing to obtain threads from, and this
// implementation creates them. The feature named `standalone' selects that. It
// is not an optimisation: in that arrangement the choice is between creating
// contexts here and not providing openkal.task at all.

#ifdef OKL_STANDALONE

// The child of a clone begins on a stack of its own with no return address, so
// the transfer cannot be written in C. The sequence is the one every C library
// uses, and it is short enough to read: the arguments are moved into the
// positions the system call takes, the function and its argument are placed on
// the child's stack, and the child calls the one with the other and then ends.
#if defined(__x86_64__)
__asm__(
".text\n"
".globl __okl_clone\n"
".hidden __okl_clone\n"
".type __okl_clone,@function\n"
"__okl_clone:\n"          // rdi=fn rsi=stack rdx=flags rcx=arg r8=ptid r9=tls, 8(%rsp)=ctid
"  mov $56,%eax\n"        // SYS_clone
"  mov %rdi,%r11\n"
"  mov %rdx,%rdi\n"       // flags
"  mov %r8,%rdx\n"        // ptid
"  mov %r9,%r8\n"         // tls
"  mov 8(%rsp),%r10\n"    // ctid
"  mov %r11,%r9\n"        // fn kept out of the way
"  and $-16,%rsi\n"
"  sub $8,%rsi\n"
"  mov %rcx,(%rsi)\n"     // arg, for the child to pop
"  syscall\n"
"  test %eax,%eax\n"
"  jnz 1f\n"
"  xor %ebp,%ebp\n"
"  pop %rdi\n"
"  call *%r9\n"
"  mov %eax,%edi\n"
"  mov $60,%eax\n"        // SYS_exit, this context only
"  syscall\n"
"  hlt\n"
"1:ret\n"
".size __okl_clone,.-__okl_clone\n"
);
#elif defined(__aarch64__)
__asm__(
".text\n"
".globl __okl_clone\n"
".hidden __okl_clone\n"
".type __okl_clone,%function\n"
"__okl_clone:\n"          // x0=fn x1=stack x2=flags x3=arg x4=ptid x5=tls x6=ctid
"  and x1,x1,#-16\n"
"  stp x0,x3,[x1,#-16]!\n"
"  uxtw x0,w2\n"
"  mov x2,x4\n"
"  mov x3,x5\n"
"  mov x4,x6\n"
"  mov x8,#220\n"         // SYS_clone
"  svc #0\n"
"  cbz x0,1f\n"
"  ret\n"
"1:ldp x1,x0,[sp],#16\n"
"  blr x1\n"
"  mov x8,#93\n"          // SYS_exit, this context only
"  svc #0\n"
".size __okl_clone,.-__okl_clone\n"
);
#endif

extern "C" okl_long __okl_clone(int (*fn)(void*), void* stack, int flags, void* arg,
                                int* ptid, void* tls, int* ctid);

#else
#include <pthread.h>
#include <sched.h>
#endif

namespace {


constexpr okl_uptr kStack = 256u * 1024u;

struct context {
    void (*entry)(void*);
    void*    arg;
    void*    stack;
    okl_uptr stack_bytes;
    okl::tls_block tls;
    volatile int tid;      // the kernel clears this when the context ends
#ifndef OKL_STANDALONE
    unsigned long thread;
#endif
};

#ifdef OKL_STANDALONE

void* alloc_bridge(okl_uptr n, okl_uptr a) { return kal_alloc(n, a); }

int run(void* p) {
    auto* c = static_cast<context*>(p);
    c->entry(c->arg);
    return 0;
}

#else

void* run(void* p) {
    auto* c = static_cast<context*>(p);
    c->entry(c->arg);
    return nullptr;
}

int translate_posix(int e) {
    switch (e) {
        case okl::e_inval: case okl::e_fault: return kal_err_invalid;
        case okl::e_again:                    return kal_err_again;
        case okl::e_nomem:                    return kal_err_no_memory;
        case okl::e_perm:                     return kal_err_permission;
        default:                              return kal_err_io;
    }
}

#endif

}  // namespace

extern "C" {

int kal_task_start(void (*entry)(void*), void* arg, kal_task* out) {
    if (entry == nullptr || out == nullptr) return kal_err_invalid;
    auto* c = static_cast<context*>(kal_alloc(sizeof(context), alignof(context)));
    if (c == nullptr) return kal_err_no_memory;
    okl::fill(c, 0, sizeof(context));
    c->entry = entry; c->arg = arg;

#ifdef OKL_STANDALONE
    c->stack_bytes = kStack;
    c->stack = kal_alloc(kStack, 16);
    c->tls   = okl::make_tls(alloc_bridge);
    if (c->stack == nullptr || c->tls.tp == nullptr) {
        if (c->stack) kal_free(c->stack, kStack, 16);
        if (c->tls.base) kal_free(c->tls.base, c->tls.bytes, c->tls.align);
        kal_free(c, sizeof(context), alignof(context));
        return kal_err_no_memory;
    }
    // The context is a thread of this process: it shares the address space,
    // the descriptors and the file system view, it is reaped without a wait,
    // and the kernel clears `tid' and wakes anything suspended upon it when
    // the context ends --- which is what kal_task_join waits for.
    constexpr int flags = 0x00000100   // CLONE_VM
                        | 0x00000200   // CLONE_FS
                        | 0x00000400   // CLONE_FILES
                        | 0x00000800   // CLONE_SIGHAND
                        | 0x00010000   // CLONE_THREAD
                        | 0x00040000   // CLONE_SYSVSEM
                        | 0x00080000   // CLONE_SETTLS
                        | 0x00100000   // CLONE_PARENT_SETTID
                        | 0x00200000;  // CLONE_CHILD_CLEARTID
    auto* top = static_cast<unsigned char*>(c->stack) + kStack;
    const okl_long r = __okl_clone(run, top, flags, c,
                                   const_cast<int*>(&c->tid), c->tls.tp,
                                   const_cast<int*>(&c->tid));
    if (okl::failed(r)) {
        kal_free(c->stack, kStack, 16);
        kal_free(c->tls.base, c->tls.bytes, c->tls.align);
        kal_free(c, sizeof(context), alignof(context));
        return okl::translate(r);
    }
#else
    pthread_t id{};
    const int rc = ::pthread_create(&id, nullptr, run, c);
    if (rc != 0) { kal_free(c, sizeof(context), alignof(context)); return translate_posix(rc); }
    c->thread = static_cast<unsigned long>(id);
#endif

    *out = kal_task{ reinterpret_cast<kal_uintptr>(c) };
    return kal_ok;
}

int kal_task_join(kal_task h) {
    auto* c = reinterpret_cast<context*>(h.h);
    if (c == nullptr) return kal_err_invalid;
#ifdef OKL_STANDALONE
    // The kernel clears the word and wakes those suspended upon it after the
    // context has left user space, so releasing its stack afterwards is safe:
    // nothing in it can still be executing.
    for (;;) {
        const int t = __atomic_load_n(&c->tid, __ATOMIC_ACQUIRE);
        if (t == 0) break;
        okl::sys(okl::nr_futex, reinterpret_cast<okl_long>(&c->tid),
                 okl::futex_wait, t, 0, 0, 0);
    }
    kal_free(c->stack, c->stack_bytes, 16);
    kal_free(c->tls.base, c->tls.bytes, c->tls.align);
#else
    const int rc = ::pthread_join(static_cast<pthread_t>(c->thread), nullptr);
    if (rc != 0) return translate_posix(rc);
#endif
    kal_free(c, sizeof(context), alignof(context));
    return kal_ok;
}

void kal_task_yield(void) { okl::sys(okl::nr_sched_yield); }

kal_uintptr kal_task_current(void) {
    // The identity is the kernel's, read once per context. It is unique among
    // contexts running at the same moment and may be reused after one ends,
    // which is what the specification says of it.
    static thread_local int cached = 0;
    if (cached == 0) cached = static_cast<int>(okl::sys(okl::nr_gettid));
    return static_cast<kal_uintptr>(cached);
}

// How many contexts can run at the same moment. Version 0.10.
//
// ADDED BECAUSE ITS ABSENCE WAS A WRONG ANSWER RATHER THAN A REFUSAL.
// `KAL_TASK_PROP_PARALLEL' says WHETHER and not HOW MANY, so a C library above
// had nowhere to look and `hardware_concurrency()' answered 1 with no error ---
// a program sizing a pool of workers got one worker and no way to know.
// Measured: 1 through openkal-musl against 32 on the same machine.
//
// THE SET THIS CONTEXT MAY RUN ON, not the set the machine has. A program
// confined to two processors is asked to size itself against two; asking the
// machine would have it size against a number it cannot use.
kal_uintptr kal_task_parallelism(void) {
    // The kernel writes a bitmap and reports how many BYTES of it it wrote.
    unsigned long mask[128] = { 0 };   // 8192 processors, which is this kernel's own bound
    const okl_long n = okl::sys(okl::nr_sched_getaffinity, 0,
                                static_cast<okl_long>(sizeof mask),
                                reinterpret_cast<okl_long>(mask));
    if (okl::failed(n) || n <= 0) return 0;   // 0 is "cannot say", and is not 1

    kal_uintptr count = 0;
    const okl_long words = n / static_cast<okl_long>(sizeof(unsigned long));
    for (okl_long i = 0; i < words; ++i)
        for (unsigned long bit = mask[i]; bit; bit &= bit - 1) ++count;
    return count;
}

// The primitive. It is the operation a caller cannot construct: the comparison
// and the suspension occur without an intervening opportunity for the value to
// change unobserved, and only the environment can arrange that.
int kal_task_wait(const kal_u32* word, kal_u32 expected,
                  kal_u64 timeout_ns) {
    okl::ktimespec ts{};
    okl_long tp = 0;
    if (timeout_ns != 0) {
        ts.sec  = static_cast<okl_i64>(timeout_ns / 1000000000u);
        ts.nsec = static_cast<okl_i64>(timeout_ns % 1000000000u);
        tp = reinterpret_cast<okl_long>(&ts);
    }
    for (;;) {
        const okl_long r = okl::sys(okl::nr_futex, reinterpret_cast<okl_long>(word),
                                    okl::futex_wait | okl::futex_private,
                                    static_cast<okl_long>(expected), tp, 0, 0);
        if (!okl::failed(r)) return kal_ok;
        // The value had already changed, which is a successful outcome: the
        // caller's condition no longer holds and it should re-examine it.
        if (r == -okl::e_again)    return kal_ok;
        if (okl::interrupted(r))   continue;
        if (r == -okl::e_timedout) return kal_err_again;
        return okl::translate(r);
    }
}

int kal_task_wake(const kal_u32* word, kal_uintptr count, kal_uintptr* woken) {
    const okl_long r = okl::sys(okl::nr_futex, reinterpret_cast<okl_long>(word),
                                okl::futex_wake | okl::futex_private,
                                static_cast<okl_long>(count), 0, 0, 0);
    if (okl::failed(r)) return okl::translate(r);
    if (woken) *woken = static_cast<kal_uintptr>(r);
    return kal_ok;
}

// The thread-local position is reported in both configurations, and it is true
// in both for different reasons: the C library's threads establish the
// convention, and so does the block this implementation builds. Clause 7.10.
kal_uintptr kal_task_props(void) {
    return KAL_TASK_PROP_PREEMPTIVE | KAL_TASK_PROP_PARALLEL
         | KAL_TASK_PROP_WAIT_TIMEOUT | KAL_TASK_PROP_THREAD_LOCAL;
}

}
