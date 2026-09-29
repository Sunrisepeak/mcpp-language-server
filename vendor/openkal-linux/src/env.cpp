#include "sys.h"
#include <openkal/env.h>

namespace okl {

// The vectors the kernel places on the stack at inception. They are recorded
// by whichever of the two entrances the program has: this implementation's own
// program entry, when the program is linked without a C library, and otherwise
// an initialiser, which every environment that starts a hosted program calls
// with the same three arguments.
//
// Reading the pseudo-file the kernel provides was the alternative. It requires
// a file system before the program has one, and reports the arguments as they
// were at inception rather than as the program received them.
int    g_argc = 0;
char** g_argv = nullptr;
char** g_envp = nullptr;
okl_ulong* g_auxv = nullptr;

void record(int argc, char** argv, char** envp) {
    g_argc = argc; g_argv = argv; g_envp = envp;
    if (envp) {
        char** e = envp;
        while (*e) ++e;
        g_auxv = reinterpret_cast<okl_ulong*>(e + 1);
    }
}

okl_ulong auxval(okl_ulong key) {
    for (okl_ulong* a = g_auxv; a && a[0]; a += 2) if (a[0] == key) return a[1];
    return 0;
}

}  // namespace okl

namespace {

// ONE C LIBRARY PASSES THESE AND ANOTHER DOES NOT, AND THE ONE THAT DOES
// NOT IS THE ONE THIS PACKAGE EXISTS TO SIT BENEATH.
//
// glibc calls every `.init_array' entry with (argc, argv, envp). musl calls
// them with NO ARGUMENTS. A function declared to take three therefore receives
// whatever the argument registers happened to hold, and this one recorded it.
//
// MEASURED 2026-08-29, WITH THE CONTROL THAT SEPARATES THE TWO EXPLANATIONS.
// It was found by running the tests for aarch64, where the first enquiry after
// the count faulted --- which reads as an architecture defect. It is not:
//
//     target                argc         argv                envp
//     x86_64-linux-gnu      1            <stack>             <stack>
//     x86_64-linux-musl     0x4004c2     1                   <stack>
//     aarch64-linux-musl    0x405ee4     1                   <stack>
//
// Both musl rows are shifted by one and the glibc row is not, so the axis is
// the C library. Running only aarch64 would have attributed it to the machine.
//
// So the arguments are CHECKED rather than believed, and where they do not hold
// the vectors are recovered from the one handle both libraries publish.
bool plausible(int argc, char* const* argv, char* const* envp) {
    if (argc < 0 || argc > 65536)   return false;
    if (argv == nullptr)            return false;
    if (envp == nullptr)            return false;
    if (argv[argc] != nullptr)      return false;   // argv is terminated at argc
    if (argc > 0 && argv[0] == nullptr) return false;
    return true;
}

// WEAK, AND DATA RATHER THAN A CALL. The independence check in this package
// forbids reaching for the C library's names, because a CALL into the runtime a
// program supplied would resolve to the program's and could re-enter this
// implementation without bound. A pointer cannot: it is read once, it executes
// nothing, and being weak it is null in a program that has no C library --- in
// which case this implementation supplied `_start' and recorded the vectors
// there, and this path is not taken.
extern "C" char** environ __attribute__((weak));

// The initial stack, whose shape is the ELF ABI's rather than any library's:
//
//     argc  argv[0] .. argv[argc-1]  NULL  envp[0] .. NULL  auxv...
//
// so from `envp' the argument vector is reached by walking back over its
// terminator. The walk is CHECKED and not trusted: the count found in the slot
// below argv[0] must equal the number of entries actually there, which a run of
// unrelated stack words does not satisfy. Where it does not hold, nothing is
// recorded and the program is told it has no arguments -- which is an answer,
// and is what clause 7.7 asks of an implementation that cannot know.
bool recover(char*** argv_out, int* argc_out, char*** envp_out) {
    char** e = environ;
    if (e == nullptr || e[-1] != nullptr) return false;
    for (long k = 0; k <= 65536; ++k) {
        auto* slot = reinterpret_cast<long*>(e - 2 - k);
        if (*slot != k) continue;
        char** candidate = e - 1 - k;
        bool holds = true;
        for (long i = 0; i < k && holds; ++i) if (candidate[i] == nullptr) holds = false;
        if (!holds || candidate[k] != nullptr) continue;
        *argv_out = candidate; *argc_out = static_cast<int>(k); *envp_out = e;
        return true;
    }
    return false;
}

// A PROGRAM ABOVE openkal SHALL NOT BE ENDED BY SOMETHING openkal NEVER
// TOLD IT ABOUT, AND WITHOUT THIS LINE ONE WAS.
//
// openkal defines no signals. `kal_stream_write' is required to REPORT that the
// far end of a stream is gone --- there is a condition for it --- and this kernel
// instead delivers SIGPIPE, whose default action ends the program. So a program
// that wrote to a closed stream did not receive the error the interface promises;
// it stopped, with a status no operation here produced and no wording anywhere in
// the specification.
//
// MEASURED THROUGH A CONSUMER, AND THE SHAPE IS WHY IT TOOK SO LONG TO SEE. A
// C library above this one answers `signal(SIGPIPE, SIG_IGN)' --- openkal has no
// signals, so the library has nothing to set and truthfully reports success. The
// program is then killed anyway, four layers below the call it made to prevent
// exactly that. Exit 141 in a test whose own assertions never printed.
//
// ⇒ Ignored HERE and not there, because here is the only place that can: the
// interface has no operation a C library could use to say it. The write then
// fails with EPIPE, which `kal_stream_write' translates and reports, which is
// what the interface said would happen all along.
//
// NOT A POLICY CHOICE ABOUT SIGNALS IN GENERAL. This is the one signal an
// ordinary openkal operation provokes; the rest are left exactly as this program
// was started with.
[[gnu::constructor(101)]] void quiet_the_signal_openkal_cannot_report() {
    // struct k_sigaction as this kernel takes it: handler, flags, restorer, mask.
    struct { void* handler; unsigned long flags; void* restorer; unsigned long mask; }
        ignore{ reinterpret_cast<void*>(1) /* SIG_IGN */, 0, nullptr, 0 };
    okl::sys(okl::nr_rt_sigaction, 13 /* SIGPIPE */,
             reinterpret_cast<okl_long>(&ignore), 0, sizeof ignore.mask);
}

[[gnu::constructor(101)]] void capture(int argc, char** argv, char** envp) {
    if (okl::g_argv != nullptr) return;
    if (plausible(argc, argv, envp)) { okl::record(argc, argv, envp); return; }
    char** rargv = nullptr; char** renvp = nullptr; int rargc = 0;
    if (recover(&rargv, &rargc, &renvp)) okl::record(rargc, rargv, renvp);
}
}  // namespace

extern "C" {

// EVERY VALUE IS COPIED INTO THE CALLER'S BUFFER. These answered with a pointer
// into this implementation's own storage, which is meaningful only while the
// implementation shares the caller's address space --- so the interface said
// something different depending on how it was reached, which is the one thing a
// contract must not do (clause 4.4).
//
// Each returns the length the value HAS, so a caller with a large enough buffer
// is done in one call, a caller that wants to size first passes a capacity of
// zero, and a caller whose buffer was too small learns it by comparing.
namespace {
kal_intptr give(const char* v, kal_uintptr n, char* out, kal_uintptr cap) {
    if (out != nullptr && cap != 0) okl::copy(out, v, n < cap ? n : cap);
    return static_cast<kal_intptr>(n);
}
}  // namespace

kal_uintptr kal_env_arg_count(void) { return static_cast<kal_uintptr>(okl::g_argc); }

kal_intptr kal_env_arg(kal_uintptr index, char* out, kal_uintptr cap) {
    if (index >= static_cast<kal_uintptr>(okl::g_argc)) return -kal_err_not_found;
    const char* s = okl::g_argv[index];
    return give(s, okl::length(s), out, cap);
}

kal_intptr kal_env_var(const char* name, kal_uintptr name_len,
                       char* out, kal_uintptr cap) {
    if (name == nullptr) return -kal_err_invalid;
    for (char** e = okl::g_envp; e && *e; ++e) {
        const char* entry = *e;
        kal_uintptr i = 0;
        while (i < name_len && entry[i] != '\0' && entry[i] == name[i]) ++i;
        if (i == name_len && entry[i] == '=') {
            const char* v = entry + name_len + 1;
            return give(v, okl::length(v), out, cap);
        }
    }
    // A name that is not there is distinct from one whose value is empty, and
    // reporting a length of zero for both would lose that.
    return -kal_err_not_found;
}

kal_uintptr kal_env_var_count(void) {
    kal_uintptr n = 0; for (char** e = okl::g_envp; e && *e; ++e) ++n; return n;
}

// The NAME at a position. The value is then obtained by kal_env_var: an
// operation answering both needs two buffers, two capacities and two lengths,
// and its second half is kal_env_var written again. The set does not change
// while the program runs, so an index may be held across the two calls.
kal_intptr kal_env_var_at(kal_uintptr index, char* out, kal_uintptr cap) {
    kal_uintptr n = 0;
    for (char** e = okl::g_envp; e && *e; ++e, ++n) {
        if (n != index) continue;
        const char* entry = *e;
        kal_uintptr i = 0; while (entry[i] != '\0' && entry[i] != '=') ++i;
        return give(entry, i, out, cap);
    }
    return -kal_err_not_found;
}

}
