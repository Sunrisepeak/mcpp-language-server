// Conversion between kal_endpoint and the kernel's socket address structures.
//
// SHARED BY openkal.net AND openkal.datagram BECAUSE THE TYPE IS. Either
// interface may be provided without the other, so the conversion belongs to
// neither; writing it twice would be one decision stated in two places, and the
// two would eventually disagree about which lengths are accepted.
#pragma once
#include "sys.h"
#include <openkal/types.h>

namespace okl {

// The port is carried in host order by kal_endpoint and in network order by the
// kernel. The conversion is written out rather than taken from a C library's
// htons, for the reason the head of sys.h gives.
inline unsigned short to_net_port(kal_u32 port) {
    const unsigned short p = static_cast<unsigned short>(port & 0xffffu);
    return static_cast<unsigned short>((p << 8) | (p >> 8));
}
inline kal_u32 from_net_port(unsigned short net) {
    return static_cast<kal_u32>((net << 8) | (net >> 8)) & 0xffffu;
}

// Fills a kernel address from an endpoint, and reports its length.
//
// A LENGTH THIS IMPLEMENTATION DOES NOT KNOW IS REFUSED RATHER THAN READ AS ONE
// IT DOES. The specification defines the set of lengths and allows it to grow;
// an implementation that ignored the field would misread every address a later
// revision defines, and would do so silently.
inline int to_kernel(const kal_endpoint& ep, ksockaddr_storage& out, okl_long& len) {
    for (auto& b : out.pad) b = 0;

    if (ep.addr_len == 4) {
        auto* v4 = reinterpret_cast<ksockaddr_in*>(&out);
        v4->family = af_inet;
        v4->port   = to_net_port(ep.port);
        okl_u32 a = 0;
        for (int i = 0; i < 4; ++i)
            a |= static_cast<okl_u32>(ep.addr[i]) << (i * 8);   // already network order
        v4->addr = a;
        for (auto& z : v4->zero) z = 0;
        len = static_cast<okl_long>(sizeof(ksockaddr_in));
        return kal_ok;
    }

    // Sixteen bytes is an address; twenty is an address followed by a scope
    // identifier, which is carried in the four bytes after it.
    if (ep.addr_len == 16 || ep.addr_len == 20) {
        auto* v6 = reinterpret_cast<ksockaddr_in6*>(&out);
        v6->family   = af_inet6;
        v6->port     = to_net_port(ep.port);
        v6->flowinfo = 0;
        for (int i = 0; i < 16; ++i) v6->addr[i] = ep.addr[i];
        okl_u32 scope = 0;
        if (ep.addr_len == 20)
            for (int i = 0; i < 4; ++i)
                scope |= static_cast<okl_u32>(ep.addr[16 + i]) << (i * 8);
        v6->scope_id = scope;
        len = static_cast<okl_long>(sizeof(ksockaddr_in6));
        return kal_ok;
    }

    return kal_err_invalid;
}

// Fills an endpoint from a kernel address. A family this implementation does
// not know leaves the endpoint zeroed and reports it, for the same reason.
inline int from_kernel(const ksockaddr_storage& in, kal_endpoint& out) {
    for (auto& b : out.addr) b = 0;
    out.addr_len = 0;
    out.port     = 0;

    if (in.family == af_inet) {
        const auto* v4 = reinterpret_cast<const ksockaddr_in*>(&in);
        const okl_u32 a = v4->addr;
        for (int i = 0; i < 4; ++i)
            out.addr[i] = static_cast<kal_u8>((a >> (i * 8)) & 0xffu);
        out.addr_len = 4;
        out.port     = from_net_port(v4->port);
        return kal_ok;
    }

    if (in.family == af_inet6) {
        const auto* v6 = reinterpret_cast<const ksockaddr_in6*>(&in);
        for (int i = 0; i < 16; ++i) out.addr[i] = v6->addr[i];
        // A zero scope identifier is reported as the shorter form. The two
        // lengths denote the same address when the scope is zero, and reporting
        // the shorter one keeps an address that came in as sixteen bytes going
        // back out as sixteen.
        if (v6->scope_id == 0) {
            out.addr_len = 16;
        } else {
            for (int i = 0; i < 4; ++i)
                out.addr[16 + i] = static_cast<kal_u8>((v6->scope_id >> (i * 8)) & 0xffu);
            out.addr_len = 20;
        }
        out.port = from_net_port(v6->port);
        return kal_ok;
    }

    return kal_err_invalid;
}

// Which socket family an endpoint asks for, or -1 for a length that is not one
// of the defined ones.
inline okl_long family_of(const kal_endpoint& ep) {
    if (ep.addr_len == 4) return af_inet;
    if (ep.addr_len == 16 || ep.addr_len == 20) return af_inet6;
    return -1;
}

}  // namespace okl
