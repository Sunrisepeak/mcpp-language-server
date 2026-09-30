#include "sys.h"
#include <openkal/abort.h>

extern "C" {

[[noreturn]] void kal_abort(const char* msg, kal_uintptr len) {
    if (msg != nullptr && len != 0) okl::write_all(2, msg, len);
    // The kernel's own means of stopping a program whose state has been
    // declared impossible. A C library's abort() raises a signal so that a
    // handler and a core dump follow; this implementation is beneath any C
    // library and raises the same signal directly.
    const okl_long tid = okl::sys(okl::nr_gettid);
    const okl_long pid = okl::sys(okl::nr_getpid);
    okl::sys(okl::nr_tgkill, pid, tid, 6 /* SIGABRT */);
    for (;;) okl::sys(okl::nr_exit_group, 127);
}

// Termination is immediate. The specification requires it, and the difference
// from a C library's exit --- which runs registered handlers and static
// destructors first --- is not observable in a small program and is observable
// in a large one, which is why it is stated rather than left to judgement.
[[noreturn]] void kal_exit(int code) {
    for (;;) okl::sys(okl::nr_exit_group, code);
}

}
