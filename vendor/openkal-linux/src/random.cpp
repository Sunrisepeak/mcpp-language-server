// openkal.random on Linux --- getrandom(2).
//
// THE KERNEL'S OWN CALL AND NOT `/dev/urandom`. The device would need a
// descriptor, which needs a path, which a capability-oriented filesystem
// deliberately does not hand out; and a program early enough in its life not to
// have a filesystem yet still has this call. `getrandom` is the interface the
// kernel offers for exactly this question.
#include "sys.h"
#include <openkal/random.h>

namespace {

// `GRND_NONBLOCK` IS NOT SET, AND THAT IS WHAT `BLOCKING` REPORTS.
//
// Without it the call waits until the pool has been initialised, which on a
// machine seconds into its first boot can be a real wait. Setting it instead
// would turn that wait into a short read — a partial success this interface
// does not have — so the wait is kept and named in the capability word.
constexpr okl_long flags_blocking = 0;

}  // namespace

extern "C" int kal_random_fill(void* out, kal_uintptr len) {
    if (len == 0) return kal_ok;
    if (out == nullptr) return kal_err_invalid;

    auto* p = static_cast<unsigned char*>(out);
    kal_uintptr filled = 0;
    while (filled < len) {
        const okl_long r = okl::sys(okl::nr_getrandom,
                                    reinterpret_cast<okl_long>(p + filled),
                                    static_cast<okl_long>(len - filled),
                                    flags_blocking);
        if (r < 0) {
            // THE BUFFER IS NOT RESTORED, AND THE CONTRACT SAYS IT NEED NOT
            // BE: a failed fill leaves the buffer unspecified rather than
            // unchanged. Restoring it would oblige this function to keep a copy
            // of what it was handed, which is a cost every successful call
            // would pay for the benefit of the failing one.
            if (r == -4 /* EINTR */) continue;
            if (r == -11 /* EAGAIN */) return kal_err_again;
            return kal_err_io;
        }
        // A short return is the kernel's, not this interface's: `getrandom`
        // caps a single call at 32 MiB. Looping is what turns it into the
        // all-or-nothing this interface promises.
        filled += static_cast<kal_uintptr>(r);
    }
    return kal_ok;
}

// Blocking, because GRND_NONBLOCK is not set above. Not hardware: the kernel's
// pool is what this reads, and whether the pool was seeded from a hardware
// source is not something this backend can observe.
extern "C" kal_uintptr kal_random_props(void) { return KAL_RANDOM_PROP_BLOCKING; }
