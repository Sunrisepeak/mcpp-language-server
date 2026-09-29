#include "sys.h"
#include <openkal/terminal.h>

// openkal.terminal upon the kernel's terminal ioctls.
//
// EVERY OPERATION IS CONDITIONED ON THE STREAM BEING ONE. TCGETS succeeds for a
// terminal and reports ENOTTY otherwise, which is the same test kal_stream_props
// performs and the same one every C library performs. Performing it here is what
// lets the interface report a refusal for a file rather than acting upon it.

namespace {

// The mode word this interface defines, read out of the kernel's local flags.
//
// KAL_TERM_LINE_EDIT IS ICANON AND NOT ICANON PLUS ECHO. The two are separate
// positions in this interface because a program that wants a password prompt
// turns off one and leaves the other, and a backend that conflated them would
// make that program's intent inexpressible.
//
// KAL_TERM_PASS_CONTROL IS READ FROM THREE FLAGS AND NOT FROM ISIG. The
// position states that the environment reserves NO keystroke, so it is set only
// where every mechanism by which this kernel reserves one is off: ISIG for the
// interrupt and its neighbours, IXON for the pair that stops and starts output,
// IEXTEN for the one that takes the next keystroke literally. A terminal upon
// which some of them had been released reads as clear and is restored to the
// set this kernel ordinarily reserves, which is the cost the specification
// records beside the position.
kal_uintptr mode_of(const okl::ktermios& t) {
    kal_uintptr m = 0;
    if ((t.lflag & okl::t_icanon) != 0) m |= KAL_TERM_LINE_EDIT;
    if ((t.lflag & okl::t_echo)   != 0) m |= KAL_TERM_ECHO;
    if ((t.lflag & (okl::t_isig | okl::t_iexten)) == 0 &&
        (t.iflag & okl::t_ixon) == 0)   m |= KAL_TERM_PASS_CONTROL;
    return m;
}

int get_termios(kal_stream s, okl::ktermios& out) {
    const okl_long r = okl::sys(okl::nr_ioctl, static_cast<okl_long>(s.h),
                                okl::tcgets, reinterpret_cast<okl_long>(&out));
    if (okl::failed(r)) {
        // ENOTTY is the answer for a stream that is not a terminal, and this
        // interface states that answer as kal_err_not_supported rather than
        // passing the kernel's own classification through.
        if (r == -okl::e_notty || r == -okl::e_inval) return kal_err_not_supported;
        return okl::translate(r);
    }
    return kal_ok;
}

}  // namespace

extern "C" {

int kal_terminal_get_mode(kal_stream s, kal_uintptr* mode) {
    if (mode == nullptr) return kal_err_invalid;
    okl::ktermios t{};
    const int rc = get_termios(s, t);
    if (rc != kal_ok) return rc;
    *mode = mode_of(t);
    return kal_ok;
}

int kal_terminal_set_mode(kal_stream s, kal_uintptr mode) {
    // READ, MODIFY, WRITE, AND NOT WRITE ALONE. The kernel's structure carries
    // input flags, output flags, a baud rate and the control characters, none of
    // which this interface names. Writing a structure this implementation
    // composed from the mode word alone would silently discard all of them ---
    // the terminal a program returned to would have a different baud rate than
    // the one it found.
    okl::ktermios t{};
    const int rc = get_termios(s, t);
    if (rc != kal_ok) return rc;
    const kal_uintptr in_effect = mode_of(t);

    if ((mode & KAL_TERM_LINE_EDIT) != 0) t.lflag |=  okl::t_icanon;
    else                                  t.lflag &= ~okl::t_icanon;
    if ((mode & KAL_TERM_ECHO) != 0)      t.lflag |=  okl::t_echo;
    else                                  t.lflag &= ~okl::t_echo;

    // A POSITION WHOSE REQUESTED VALUE IS THE ONE IN EFFECT IS NOT WRITTEN, and
    // this position is why the specification states the rule: it stands for
    // three of the kernel's flags, so establishing it again would settle two
    // mechanisms the caller never asked about. A user who had released the
    // keystroke that stops output keeps it released while a program turns the
    // echo off and back on.
    if (((mode ^ in_effect) & KAL_TERM_PASS_CONTROL) != 0) {
        if ((mode & KAL_TERM_PASS_CONTROL) != 0) {
            t.lflag &= ~(okl::t_isig | okl::t_iexten);
            t.iflag &= ~okl::t_ixon;
        } else {
            t.lflag |=  (okl::t_isig | okl::t_iexten);
            t.iflag |=   okl::t_ixon;
        }
    }

    // AND A MODE IS NOT A WAY TO END THE INPUT. With line assembly off, how
    // long a read waits is decided by VMIN and VTIME rather than by a newline,
    // and a terminal left at VMIN=0 by whatever ran before makes
    // `kal_stream_read' report zero --- which clause 7.4 says denotes the end
    // of the input. The least count is therefore established together with the
    // mode that needs it; a caller that wants a read which gives up asks
    // `kal_timeout_read' for one.
    if ((mode & KAL_TERM_LINE_EDIT) == 0) {
        t.cc[okl::v_min]  = 1;
        t.cc[okl::v_time] = 0;
    }

    // A position this implementation does not distinguish is ignored rather
    // than refused, which is what clause 6.2 requires of a word: a program
    // compiled against a later revision sets a position this build has never
    // heard of, and refusing would make that program fail against an
    // implementation that is behaving correctly.
    const okl_long w = okl::sys(okl::nr_ioctl, static_cast<okl_long>(s.h),
                                okl::tcsets, reinterpret_cast<okl_long>(&t));
    if (okl::failed(w)) {
        if (w == -okl::e_notty || w == -okl::e_inval) return kal_err_not_supported;
        return okl::translate(w);
    }
    return kal_ok;
}

int kal_terminal_size(kal_stream s, kal_uintptr* cols, kal_uintptr* rows) {
    if (cols == nullptr || rows == nullptr) return kal_err_invalid;
    okl::kwinsize w{};
    const okl_long r = okl::sys(okl::nr_ioctl, static_cast<okl_long>(s.h),
                                okl::tiocgwinsz, reinterpret_cast<okl_long>(&w));
    if (okl::failed(r)) {
        // BOTH OUTPUTS ARE LEFT UNTOUCHED, which the interface requires. A
        // serial line answers ENOTTY here while answering TCGETS, so this is not
        // the same condition as "not a terminal" and the outputs must survive
        // it for a caller to distinguish them.
        if (r == -okl::e_notty || r == -okl::e_inval) return kal_err_not_supported;
        return okl::translate(r);
    }
    *cols = static_cast<kal_uintptr>(w.col);
    *rows = static_cast<kal_uintptr>(w.row);
    return kal_ok;
}

kal_uintptr kal_terminal_props(kal_stream s) {
    kal_uintptr p = 0;

    okl::ktermios t{};
    if (get_termios(s, t) == kal_ok) p |= KAL_TERM_PROP_MODE;

    // THE SIZE POSITION IS ASKED FOR RATHER THAN ASSUMED FROM THE FIRST. A
    // pseudo terminal answers both; a serial line answers TCGETS and not
    // TIOCGWINSZ. Deriving one from the other would make the word claim a
    // facility the very next call refuses, which is the disagreement clause 6.2
    // exists to prevent.
    okl::kwinsize w{};
    const okl_long r = okl::sys(okl::nr_ioctl, static_cast<okl_long>(s.h),
                                okl::tiocgwinsz, reinterpret_cast<okl_long>(&w));
    if (!okl::failed(r)) p |= KAL_TERM_PROP_SIZE;

    return p;
}

}  // extern "C"
