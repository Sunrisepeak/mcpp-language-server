#include "sys.h"
#include "handle.h"
#include "endpoint.h"
#include <openkal/net.h>

// openkal.net upon the kernel's socket calls.
//
// A CONNECTION IS AN OWNED HANDLE AND THE STREAM IS BORROWED FROM IT, exactly as
// kal_file and kal_fs_stream are here. The owned handle carries a generation so
// that a released one stops being valid, which clause 7.2 requires; the stream
// it yields is the bare descriptor, because that is what openkal.stream's
// transfer operations take. The interface was changed to this shape after the
// first form --- an owned kal_stream --- turned out not to be implementable
// under clause 7.2 at all.

namespace {

// Both handle kinds are descriptors, and both use the packing in handle.h. The
// generation makes a released handle stop being valid, which clause 7.2
// requires and which a bare descriptor could not provide: the kernel reuses the
// lowest free number, so a stale word would name whatever was opened next.
int fd_of(kal_net_conn c)     { return okl::unpack(c.h); }
int fd_of(kal_net_listener l) { return okl::unpack(l.h); }

int report_address(okl_long call, int fd, kal_endpoint* out) {
    if (out == nullptr) return kal_err_invalid;
    if (fd < 0) return kal_err_invalid;
    okl::ksockaddr_storage ss{};
    okl_long len = static_cast<okl_long>(sizeof ss);
    const okl_long r = okl::sys(call, fd, reinterpret_cast<okl_long>(&ss),
                                reinterpret_cast<okl_long>(&len));
    if (okl::failed(r)) return okl::translate(r);
    return okl::from_kernel(ss, *out);
}

}  // namespace

extern "C" {

int kal_net_connect(const kal_endpoint* to, kal_net_conn* out) {
    if (to == nullptr || out == nullptr) return kal_err_invalid;
    const okl_long family = okl::family_of(*to);
    if (family < 0) return kal_err_invalid;

    okl::ksockaddr_storage ss{};
    okl_long len = 0;
    if (const int rc = okl::to_kernel(*to, ss, len); rc != kal_ok) return rc;

    const okl_long fd = okl::sys(okl::nr_socket, family,
                                 okl::sock_stream | okl::sock_cloexec,
                                 okl::ipproto_tcp);
    if (okl::failed(fd)) return okl::translate(fd);

    for (;;) {
        const okl_long r = okl::sys(okl::nr_connect, fd,
                                    reinterpret_cast<okl_long>(&ss), len);
        if (okl::interrupted(r)) continue;
        if (okl::failed(r)) {
            okl::sys(okl::nr_close, fd);
            return okl::translate(r);
        }
        break;
    }

    out->h = okl::pack(static_cast<int>(fd));
    if (out->h == 0) { okl::sys(okl::nr_close, fd); return kal_err_no_memory; }
    return kal_ok;
}

int kal_net_listen(const kal_endpoint* local, kal_net_listener* out) {
    if (local == nullptr || out == nullptr) return kal_err_invalid;
    const okl_long family = okl::family_of(*local);
    if (family < 0) return kal_err_invalid;

    okl::ksockaddr_storage ss{};
    okl_long len = 0;
    if (const int rc = okl::to_kernel(*local, ss, len); rc != kal_ok) return rc;

    const okl_long fd = okl::sys(okl::nr_socket, family,
                                 okl::sock_stream | okl::sock_cloexec,
                                 okl::ipproto_tcp);
    if (okl::failed(fd)) return okl::translate(fd);

    // SO_REUSEADDR, because a listener whose predecessor is in the kernel's
    // lingering state would otherwise be refused for a reason that has nothing
    // to do with the caller. A program restarted within the linger interval is
    // the ordinary case, not an unusual one.
    {
        const int on = 1;
        okl::sys(okl::nr_setsockopt, fd, okl::sol_socket, okl::so_reuseaddr,
                 reinterpret_cast<okl_long>(&on),
                 static_cast<okl_long>(sizeof on));
    }

    if (const okl_long r = okl::sys(okl::nr_bind, fd,
                                    reinterpret_cast<okl_long>(&ss), len);
        okl::failed(r)) {
        okl::sys(okl::nr_close, fd);
        return okl::translate(r);
    }

    // The backlog the kernel is asked for. A number rather than a name, because
    // this interface does not expose one and a caller has no way to state it.
    if (const okl_long r = okl::sys(okl::nr_listen, fd, 128); okl::failed(r)) {
        okl::sys(okl::nr_close, fd);
        return okl::translate(r);
    }

    out->h = okl::pack(static_cast<int>(fd));
    if (out->h == 0) { okl::sys(okl::nr_close, fd); return kal_err_no_memory; }
    return kal_ok;
}

int kal_net_accept(kal_net_listener l, kal_net_conn* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = fd_of(l);
    if (fd < 0) return kal_err_invalid;

    for (;;) {
        const okl_long r = okl::sys(okl::nr_accept4, fd, 0, 0,
                                    okl::sock_cloexec);
        if (okl::interrupted(r)) continue;
        if (okl::failed(r)) return okl::translate(r);
        out->h = okl::pack(static_cast<int>(r));
        if (out->h == 0) { okl::sys(okl::nr_close, r); return kal_err_no_memory; }
        return kal_ok;
    }
}

kal_stream kal_net_stream(kal_net_conn c) {
    // The bare descriptor, for the reason kal_fs_stream gives: openkal.stream's
    // operations take whatever the environment's transfer calls take, and a
    // packed word is not that. It carries its type, so a handle crossing
    // between two interfaces is not a word either of them has to interpret.
    const int fd = fd_of(c);
    return kal_stream{ fd < 0 ? 0u : static_cast<kal_uintptr>(fd) };
}

int kal_net_peer(kal_net_conn c, kal_endpoint* out) {
    return report_address(okl::nr_getpeername, fd_of(c), out);
}

int kal_net_local(kal_net_conn c, kal_endpoint* out) {
    return report_address(okl::nr_getsockname, fd_of(c), out);
}

int kal_net_listener_local(kal_net_listener l, kal_endpoint* out) {
    return report_address(okl::nr_getsockname, fd_of(l), out);
}

int kal_net_shutdown(kal_net_conn c, int direction) {
    const int fd = fd_of(c);
    if (fd < 0) return kal_err_invalid;

    // The kernel numbers the directions from zero and this interface from one,
    // so the mapping is written out rather than arithmetic upon the argument. A
    // direction this interface does not define is refused rather than passed
    // through, because the kernel would read an unknown number as SHUT_RD.
    okl_long how;
    switch (direction) {
        case KAL_SHUT_READ:  how = 0; break;
        case KAL_SHUT_WRITE: how = 1; break;
        case KAL_SHUT_BOTH:  how = 2; break;
        default: return kal_err_invalid;
    }

    const okl_long r = okl::sys(okl::nr_shutdown, fd, how);
    if (okl::failed(r)) return okl::translate(r);
    return kal_ok;
}

void kal_net_close(kal_net_conn c) {
    const int fd = fd_of(c);
    if (fd < 0) return;
    okl::sys(okl::nr_close, fd);
    okl::retire(c.h);
}

void kal_net_close_listener(kal_net_listener l) {
    const int fd = fd_of(l);
    if (fd < 0) return;
    okl::sys(okl::nr_close, fd);
    okl::retire(l.h);
}

// Both positions hold on this kernel. IPv6 is configurable out of a build, but
// the socket call then reports it at the point of the attempt, and a word that
// claimed less than the kernel offers would withhold a facility a caller could
// have used.
kal_uintptr kal_net_props(void) { return KAL_NET_PROP_IPV6 | KAL_NET_PROP_HALFCLOSE; }

}  // extern "C"
