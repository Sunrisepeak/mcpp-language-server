#include "sys.h"
#include "handle.h"
#include "endpoint.h"
#include <openkal/timeout.h>

// openkal.timeout upon ppoll(2) and wait4(2).
//
// THE BOUND IS APPLIED BEFORE THE OPERATION AND NOT DURING IT. ppoll reports
// whether a descriptor would transfer without blocking, so a bounded read is a
// bounded wait for readiness followed by the ordinary read. This is what the
// environment already does at the point of the call, which is why clause 6.3
// records readiness notification as the alternative that was NOT adopted: an
// interface reporting readiness would oblige an implementation to maintain a set
// and a context of its own, and this one obliges it to maintain nothing.
//
// The bound is therefore upon the WAIT and not upon the transfer. A read that
// becomes ready within the bound and then transfers slowly is not interrupted,
// which is the behaviour every environment's own bounded read has.

namespace {

// A duration of zero denotes no bound, which is the convention kal_task_wait
// establishes. ppoll expresses that by being given no timespec at all.
const okl::ktimespec* bound_of(kal_u64 ns, okl::ktimespec& storage) {
    if (ns == 0) return nullptr;
    storage.sec  = static_cast<okl_i64>(ns / 1000000000ull);
    storage.nsec = static_cast<okl_i64>(ns % 1000000000ull);
    return &storage;
}

// Waits for one descriptor. Reports kal_ok when it is ready, kal_err_again when
// the bound expired, and a translated error otherwise.
int await(int fd, short events, kal_u64 ns) {
    okl::kpollfd p{ fd, events, 0 };
    okl::ktimespec ts{};
    const okl::ktimespec* to = bound_of(ns, ts);

    for (;;) {
        const okl_long r = okl::sys(okl::nr_ppoll, reinterpret_cast<okl_long>(&p),
                                    1, reinterpret_cast<okl_long>(to), 0, 0);
        // AN INTERRUPTED WAIT IS NOT RETRIED WITH THE WHOLE BOUND AGAIN.
        //
        // Retrying with the original duration would make the bound restart at
        // every signal, so a program on a system that delivers them regularly
        // would wait without end while appearing to be bounded. ppoll leaves the
        // caller's timespec untouched, so there is nothing to resume from, and
        // the honest report is that the operation did not complete.
        if (okl::interrupted(r)) return kal_err_again;
        if (okl::failed(r)) return okl::translate(r);
        if (r == 0) return kal_err_again;   // the bound expired
        return kal_ok;
    }
}

// A STREAM HANDLE IS A DESCRIPTOR AND IS NOT DECODED. stream.cpp states it in
// terms, kal_stream_read and kal_stream_write take it as one, and so must the
// wait that precedes them: the wait and the transfer that follows it have to
// name the same object or the wait answers about something else.
//
// THIS FILE USED TO DECODE IT AS AN OWNED HANDLE, AND THE DECODE SUCCEEDED.
// handle.h packs an owned handle as (generation << 32) | (fd + 1), so a bare
// descriptor N has exactly the shape of a packed handle naming N-1, and
// okl::unpack accepts it whenever generation N-1 is still zero -- which is
// every index at which no owned handle has yet been released. The wait was
// therefore performed upon descriptor N-1 and the transfer upon N.
//
// The former reading tested `unpack` and fell back when it failed, on the
// stated ground that the standard streams are not packed. That ground is
// correct and the conclusion drawn from it was not: NO stream handle is packed
// here, and of the three standard ones only kal_stdin, whose handle is zero,
// fails to decode. kal_stdout decoded to descriptor 0 and kal_stderr to 1.
//
// What a caller observed was an expiry for a stream that had bytes waiting, or
// a wait without bound inside an operation that states one, according to what
// happened to occupy the descriptor below. Both are functions of the process's
// descriptor history, so the same program answered differently when run alone
// and when run after something else -- and the index healed for good once an
// owned handle at N-1 had been released, because that advances the generation
// and makes the decode fail correctly.
int stream_fd(kal_stream s) { return static_cast<int>(s.h); }

}  // namespace

extern "C" {

kal_intptr kal_timeout_read(kal_stream s, void* buf, kal_uintptr len, kal_u64 ns) {
    // A transfer of zero bytes does not wait and is not bounded. Waiting first
    // would turn a call that always succeeds into one that can expire.
    if (len == 0) return 0;

    if (const int rc = await(stream_fd(s), okl::poll_in, ns); rc != kal_ok) return -rc;
    return kal_stream_read(s, buf, len);
}

kal_intptr kal_timeout_write(kal_stream s, const void* buf, kal_uintptr len, kal_u64 ns) {
    if (len == 0) return 0;

    if (const int rc = await(stream_fd(s), okl::poll_out, ns); rc != kal_ok) return -rc;
    return kal_stream_write(s, buf, len);
}

// THE TWO OPERATIONS BELOW DO DECODE, AND THAT IS NOT AN INCONSISTENCY WITH THE
// TWO ABOVE. A listener and a datagram are owned: their handles are made by
// okl::pack and released by okl::retire, so the decode is the operation that
// recovers the descriptor and its failure is how a released handle is refused.
// A stream is borrowed and carries no generation. The four call sites divide
// exactly along that line, and the two that were wrong were the two that had a
// borrowed handle in hand.
int kal_timeout_accept(kal_net_listener l, kal_u64 ns, kal_net_conn* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = okl::unpack(l.h);
    if (fd < 0) return kal_err_invalid;

    if (const int rc = await(fd, okl::poll_in, ns); rc != kal_ok) return rc;
    return kal_net_accept(l, out);
}

kal_intptr kal_timeout_recv_from(kal_datagram d, void* buf, kal_uintptr len,
                                 kal_endpoint* from, kal_u64 ns) {
    const int fd = okl::unpack(d.h);
    if (fd < 0) return -kal_err_invalid;

    if (const int rc = await(fd, okl::poll_in, ns); rc != kal_ok) return -rc;
    return kal_datagram_recv_from(d, buf, len, from);
}

int kal_timeout_wait_process(kal_process p, kal_u64 ns, int* status, int* terminated) {
    if (p.h == 0) return kal_err_invalid;

    // WNOHANG AND A POLLING LOOP, BECAUSE THE KERNEL HAS NO BOUNDED WAIT FOR A
    // CHILD. wait4 either blocks or does not wait at all; there is no timespec.
    //
    // The alternative is a signal handler for SIGCHLD, which this implementation
    // does not have and would not want: a handler is process-wide state, and an
    // implementation that installed one would take a facility away from the
    // program above it. The loop is what the environment permits, and the
    // interval is bounded below by the granularity this interface reports so
    // that the polling cost is stated rather than hidden.
    constexpr okl_u64 interval_ns = 1000000ull;   // one millisecond
    okl_u64 waited = 0;

    for (;;) {
        int st = 0;
        const okl_long r = okl::sys(okl::nr_wait4, static_cast<okl_long>(p.h),
                                    reinterpret_cast<okl_long>(&st),
                                    1 /* WNOHANG */, 0);
        if (okl::interrupted(r)) continue;
        if (okl::failed(r)) return okl::translate(r);

        if (r != 0) {
            const int signalled = st & 0x7f;
            if (signalled == 0) {
                if (status) *status = (st >> 8) & 0xff;
                if (terminated) *terminated = 0;
            } else {
                if (status) *status = signalled;
                if (terminated) *terminated = 1;
            }
            return kal_ok;
        }

        if (ns != 0 && waited >= ns) return kal_err_again;

        okl::ktimespec ts{ 0, static_cast<okl_i64>(interval_ns) };
        okl::sys(okl::nr_nanosleep, reinterpret_cast<okl_long>(&ts), 0);
        waited += interval_ns;
    }
}

// The kernel states a bound in nanoseconds and the clock advances at the
// scheduler's resolution, so a bound finer than a tick is rounded up by the
// scheduler rather than refused. One millisecond is reported because it is the
// interval the child-waiting loop above polls at, and a caller asking for less
// than the coarsest of the operations here would otherwise be told a number one
// of them cannot meet.
kal_u64 kal_timeout_granularity(void) { return 1000000u; }

}  // extern "C"
