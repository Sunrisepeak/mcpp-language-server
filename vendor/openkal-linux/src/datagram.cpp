#include "sys.h"
#include "handle.h"
#include "endpoint.h"
#include <openkal/datagram.h>

// openkal.datagram upon the kernel's socket calls.
//
// A DATAGRAM IS NOT PACKED AS A kal_stream, and the handle type is its own for
// that reason: kal_stream_read reports a count and not a boundary, so reading a
// datagram through it would lose the property that distinguishes this interface.
// The packing is the same, the type is not, and the type is what prevents the
// mistake.

namespace {

int fd_of(kal_datagram d) { return okl::unpack(d.h); }

}  // namespace

extern "C" {

int kal_datagram_open(const kal_endpoint* local, kal_datagram* out) {
    if (out == nullptr) return kal_err_invalid;

    // A null local endpoint asks for one that may send and whose receiving
    // address is unspecified. IPv4 is chosen for it, because a family must be
    // named at the point the socket is made and this is the one every
    // environment that has a network at all provides.
    okl_long family = okl::af_inet;
    if (local != nullptr) {
        family = okl::family_of(*local);
        if (family < 0) return kal_err_invalid;
    }

    const okl_long fd = okl::sys(okl::nr_socket, family,
                                 okl::sock_dgram | okl::sock_cloexec,
                                 okl::ipproto_udp);
    if (okl::failed(fd)) return okl::translate(fd);

    if (local != nullptr) {
        okl::ksockaddr_storage ss{};
        okl_long len = 0;
        if (const int rc = okl::to_kernel(*local, ss, len); rc != kal_ok) {
            okl::sys(okl::nr_close, fd);
            return rc;
        }
        if (const okl_long r = okl::sys(okl::nr_bind, fd,
                                        reinterpret_cast<okl_long>(&ss), len);
            okl::failed(r)) {
            okl::sys(okl::nr_close, fd);
            return okl::translate(r);
        }
    }

    out->h = okl::pack(static_cast<int>(fd));
    if (out->h == 0) { okl::sys(okl::nr_close, fd); return kal_err_no_memory; }
    return kal_ok;
}

int kal_datagram_local(kal_datagram d, kal_endpoint* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = fd_of(d);
    if (fd < 0) return kal_err_invalid;

    okl::ksockaddr_storage ss{};
    okl_long len = static_cast<okl_long>(sizeof ss);
    const okl_long r = okl::sys(okl::nr_getsockname, fd,
                                reinterpret_cast<okl_long>(&ss),
                                reinterpret_cast<okl_long>(&len));
    if (okl::failed(r)) return okl::translate(r);
    return okl::from_kernel(ss, *out);
}

kal_intptr kal_datagram_send_to(kal_datagram d, const void* buf, kal_uintptr len,
                                const kal_endpoint* to) {
    const int fd = fd_of(d);
    if (fd < 0 || to == nullptr) return -kal_err_invalid;

    okl::ksockaddr_storage ss{};
    okl_long addrlen = 0;
    if (const int rc = okl::to_kernel(*to, ss, addrlen); rc != kal_ok)
        return -rc;

    for (;;) {
        const okl_long r = okl::sys(okl::nr_sendto, fd,
                                    reinterpret_cast<okl_long>(buf),
                                    static_cast<okl_long>(len), 0,
                                    reinterpret_cast<okl_long>(&ss), addrlen);
        if (okl::interrupted(r)) continue;
        if (okl::failed(r)) return -okl::translate(r);

        // A MESSAGE IS SENT WHOLE OR NOT AT ALL, which is what this interface
        // states. The kernel reports a count anyway; a count short of the length
        // would mean the medium had split the message, which for a datagram
        // socket it does not do. Reporting the short count as success would give
        // a caller a partial send this interface says cannot occur, so it is
        // reported as a failure of the medium instead.
        const kal_uintptr n = static_cast<kal_uintptr>(r);
        return n == len ? static_cast<kal_intptr>(n) : -kal_err_io;
    }
}

kal_intptr kal_datagram_recv_from(kal_datagram d, void* buf, kal_uintptr len,
                                  kal_endpoint* from) {
    const int fd = fd_of(d);
    if (fd < 0) return -kal_err_invalid;

    okl::ksockaddr_storage ss{};
    okl_long addrlen = static_cast<okl_long>(sizeof ss);

    for (;;) {
        const okl_long r = okl::sys(okl::nr_recvfrom, fd,
                                    reinterpret_cast<okl_long>(buf),
                                    static_cast<okl_long>(len), 0,
                                    reinterpret_cast<okl_long>(&ss),
                                    reinterpret_cast<okl_long>(&addrlen));
        if (okl::interrupted(r)) continue;
        if (okl::failed(r)) return -okl::translate(r);

        // THE COUNT REPORTED IS WHAT WAS PLACED IN THE BUFFER, not what was
        // sent. Without MSG_TRUNC the kernel already reports the former, which
        // is what this interface requires: a caller that trusted the larger
        // number would read beyond its own buffer.
        if (from != nullptr) {
            // A sender whose family this implementation does not know leaves the
            // endpoint zeroed rather than partly filled. The transfer still
            // happened and is reported; what is unknown is who sent it.
            if (okl::from_kernel(ss, *from) != kal_ok) {
                for (auto& b : from->addr) b = 0;
                from->addr_len = 0;
                from->port     = 0;
            }
        }
        return static_cast<kal_intptr>(r);
    }
}

void kal_datagram_close(kal_datagram d) {
    const int fd = fd_of(d);
    if (fd < 0) return;
    okl::sys(okl::nr_close, fd);
    okl::retire(d.h);
}

// Broadcast is not claimed. The kernel provides it only after SO_BROADCAST has
// been set, and this interface has no operation that would set it; a word
// claiming a facility no operation reaches is the disagreement clause 6.2 exists
// to prevent.
kal_uintptr kal_datagram_props(void) { return KAL_DGRAM_PROP_IPV6; }

}  // extern "C"
