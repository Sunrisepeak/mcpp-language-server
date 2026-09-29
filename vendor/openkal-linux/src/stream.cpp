#include "sys.h"
#include <openkal/stream.h>

extern "C" {

// THE STREAMS ARE THIS ENVIRONMENT'S OWN DESCRIPTORS AND ARE NOT PACKED, which
// is what `openkal.stream`'s transfer operations take and what makes a stream
// this implementation hands out interchangeable with one it received.
//
// ONE CONSEQUENCE, AND IT IS NOT VISIBLE FROM THIS FILE. Standard input is
// therefore the handle ZERO, and `kal_spawn_streams` reserves zero to mean
// "the stream the parent has" (openkal/process.h). The two readings agree at
// position `in` --- placing standard input at standard input and inheriting it
// are the same act --- and cannot be told apart anywhere else, so a caller that
// places its own standard input at position `out` is asking for something that
// structure cannot express.
//
// The specification records the collision and what a caller should do about it;
// openkal-musl refuses that spawn rather than passing on a word that would be
// read as inheritance. An implementation MAY remove the ambiguity for every
// caller by not answering any stream enquiry with zero, and this one does not:
// packing these three would make a stream this implementation hands out
// different in kind from the descriptors `kal_fs_stream` and
// `kal_process_channel` report, and the interface has no operation that would
// unpack it for the caller.
kal_stream kal_stdin (void) { return kal_stream{0}; }
kal_stream kal_stdout(void) { return kal_stream{1}; }
kal_stream kal_stderr(void) { return kal_stream{2}; }

// ONE SIGNED WORD: the count, or the negated condition when no byte moved.
// Clause 5.2.1. A caller that moved some bytes and then met a condition is told
// how many moved and meets the condition on its next call, which is what every
// consumer of the earlier two-word form did with it by hand.
kal_intptr kal_stream_write(kal_stream s, const void* buf, kal_uintptr len) {
    const auto* p = static_cast<const unsigned char*>(buf);
    kal_uintptr done = 0;
    while (done < len) {
        const okl_long r = okl::sys(okl::nr_write, static_cast<okl_long>(s.h),
                                    reinterpret_cast<okl_long>(p + done),
                                    static_cast<okl_long>(len - done));
        // An interrupted call is retried rather than reported. Clause 7.5: a
        // caller cannot distinguish this condition from a genuine failure
        // without knowledge of the environment, and an implementation that
        // reports it produces short writes on any system that delivers
        // signals --- a failure a test suite is unlikely to reproduce.
        if (okl::interrupted(r)) continue;
        if (okl::failed(r)) {
            if (done != 0) return static_cast<kal_intptr>(done);
            return -okl::translate(r);
        }
        if (r == 0) break;
        done += static_cast<kal_uintptr>(r);
    }
    return static_cast<kal_intptr>(done);
}

kal_intptr kal_stream_read(kal_stream s, void* buf, kal_uintptr len) {
    for (;;) {
        const okl_long r = okl::sys(okl::nr_read, static_cast<okl_long>(s.h),
                                    reinterpret_cast<okl_long>(buf),
                                    static_cast<okl_long>(len));
        if (okl::interrupted(r)) continue;
        if (okl::failed(r)) return -okl::translate(r);
        // A short read is reported as it occurred. Unlike a short write it
        // carries information the caller requires: zero denotes end of input.
        return static_cast<kal_intptr>(r);
    }
}

int kal_stream_flush(kal_stream s) {
    // Nothing is buffered at this level, so there is nothing to commit for a
    // stream that is not a file. For one that is, the durability the caller
    // asked for is the kernel's to provide, and a failure to provide it is
    // reported rather than concealed. A descriptor that cannot be synchronised
    // --- a terminal, a pipe --- reports that, and reporting it as a failure
    // would make every caller distinguish it from a real one.
    const okl_long r = okl::sys(okl::nr_fsync, static_cast<okl_long>(s.h));
    if (!okl::failed(r)) return kal_ok;
    if (r == -okl::e_inval || r == -okl::e_notty || r == -okl::e_badf) return kal_ok;
    return okl::translate(r);
}

kal_uintptr kal_stream_props(kal_stream s) {
    // The enquiry the kernel offers is an attempt to read a terminal's
    // settings: it succeeds for a terminal and reports ENOTTY otherwise. This
    // is the same test every C library performs, and it is performed here so
    // that the library above need not know which environment it is upon.
    unsigned char termios[64] = { 0 };
    const okl_long r = okl::sys(okl::nr_ioctl, static_cast<okl_long>(s.h),
                                0x5401 /* TCGETS */,
                                reinterpret_cast<okl_long>(termios));
    return okl::failed(r) ? kal_uintptr{0} : KAL_STREAM_PROP_INTERACTIVE;
}

}
