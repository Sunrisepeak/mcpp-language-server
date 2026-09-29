#include "sys.h"
#include <openkal/time.h>

namespace {
kal_duration nanoseconds(const okl::ktimespec& t) {
    return static_cast<kal_duration>(t.sec) * 1000000000u
         + static_cast<kal_duration>(t.nsec);
}
}  // namespace

extern "C" {

kal_duration kal_time_monotonic(void) {
    okl::ktimespec t{};
    okl::sys(okl::nr_clock_gettime, okl::clock_monotonic, reinterpret_cast<okl_long>(&t));
    return nanoseconds(t);
}

kal_duration kal_time_wall(void) {
    okl::ktimespec t{};
    okl::sys(okl::nr_clock_gettime, okl::clock_realtime, reinterpret_cast<okl_long>(&t));
    return nanoseconds(t);
}

kal_duration kal_time_monotonic_granularity(void) {
    okl::ktimespec t{};
    if (okl::failed(okl::sys(okl::nr_clock_getres, okl::clock_monotonic,
                             reinterpret_cast<okl_long>(&t)))) return 1;
    const kal_duration ns = nanoseconds(t);
    return ns == 0 ? 1 : ns;
}

void kal_time_sleep(kal_duration ns) {
    okl::ktimespec req{ static_cast<okl_i64>(ns / 1000000000u),
                        static_cast<okl_i64>(ns % 1000000000u) };
    // The specification requires that the call not return early, so an
    // interruption resumes the remainder rather than reporting it. The kernel
    // writes what is left into the same structure, which is why it is not
    // const.
    for (;;) {
        const okl_long r = okl::sys(okl::nr_nanosleep, reinterpret_cast<okl_long>(&req),
                                    reinterpret_cast<okl_long>(&req));
        if (!okl::interrupted(r)) return;
    }
}

// The monotonic clock of this kernel does not advance while the machine is
// suspended, which the corresponding property records.
kal_uintptr kal_time_props(void) {
    return KAL_TIME_PROP_WALL_AVAILABLE | KAL_TIME_PROP_MONOTONIC_SUSPENDS
         | KAL_TIME_PROP_SLEEP_PRECISE;
}

}
