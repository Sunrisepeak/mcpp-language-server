#include "sys.h"
#include "handle.h"
#include <openkal/fs.h>
#include <openkal/memory.h>

namespace {


// The directories this implementation supplies. A hosted system does not
// confine an ordinary program, so it supplies both the working directory and
// the whole file system; an implementation that confined a program would
// supply fewer, and the program could not tell the difference except by
// finding that a directory it wanted was absent.
//
// The working directory is reported under the name the environment knows it
// by, which is its absolute path rather than ".". A C library above openkal
// must both resolve an absolute path and report one, and a name of "." leaves
// it able to do only the first.
struct preopen { const char* name; kal_uintptr len; okl_uptr handle; };

char g_cwd[4096];

preopen* table(kal_uintptr* count) {
    static preopen t[2];
    static bool opened = false;
    if (!opened) {
        opened = true;

        const okl_long n = okl::sys(okl::nr_getcwd, reinterpret_cast<okl_long>(g_cwd),
                                    static_cast<okl_long>(sizeof g_cwd));
        // getcwd reports the length including the terminator, and reports a
        // failure when the directory has been removed. A program whose working
        // directory no longer has a name still has the directory, so the
        // handle is opened either way and only the name falls back.
        okl_uptr cwd_len = 0;
        if (!okl::failed(n) && n > 1) cwd_len = static_cast<okl_uptr>(n) - 1;
        else { g_cwd[0] = '.'; g_cwd[1] = '\0'; cwd_len = 1; }

        const okl_long fd0 = okl::sys(okl::nr_openat, okl::at_fdcwd,
                                      reinterpret_cast<okl_long>("."),
                                      okl::o_rdonly | okl::o_directory | okl::o_cloexec, 0);
        const okl_long fd1 = okl::sys(okl::nr_openat, okl::at_fdcwd,
                                      reinterpret_cast<okl_long>("/"),
                                      okl::o_rdonly | okl::o_directory | okl::o_cloexec, 0);
        t[0] = { g_cwd, cwd_len, okl::failed(fd0) ? 0u : okl::pack(static_cast<int>(fd0)) };
        t[1] = { "/",   1,       okl::failed(fd1) ? 0u : okl::pack(static_cast<int>(fd1)) };
    }
    if (count) *count = 2;
    return t;
}

int kind_of(okl_u32 mode) {
    switch (mode & okl::s_ifmt) {
        case okl::s_ifreg: return kal_node_file;
        case okl::s_ifdir: return kal_node_directory;
        case okl::s_iflnk: return kal_node_link;
        default:           return kal_node_other;
    }
}

// The properties of the volume a descriptor is on. kal_fs_props answers with
// this, and so does the one field of an enquiry that is a volume's property.
kal_uintptr volume_props(int fd) {
    // LOCKS AND CAPACITY ARE IN THE CONSERVATIVE SET, and that is a claim
    // about this kernel rather than about the volume: an open-file lock and
    // `fstatfs' are answered by the VFS for every format beneath it, including
    // the read-only ones --- a lock excludes writers a read-only volume does not
    // have, which is a true answer and not a useful one. A format that could not
    // would have to be excluded by name here, and this kernel has none.
    const kal_uintptr conservative =
        KAL_FS_PROP_MODIFIED_TIME | KAL_FS_PROP_ATOMIC_RENAME
        | KAL_FS_PROP_LOCKS | KAL_FS_PROP_CAPACITY;
    if (fd < 0) return 0;

    okl::kstatfs sf{};
    const okl_long r = okl::sys(okl::nr_fstatfs, fd, reinterpret_cast<okl_long>(&sf));
    if (okl::failed(r)) return conservative;

    switch (sf.f_type) {
        // Formats with a case-sensitive namespace and nodes that name others.
        case okl::fs_ext234: case okl::fs_btrfs: case okl::fs_xfs:
        case okl::fs_f2fs:   case okl::fs_tmpfs: case okl::fs_overlay:
        case okl::fs_zfs:    case okl::fs_bcachefs:
            return conservative | KAL_FS_PROP_CASE_SENSITIVE
                 | KAL_FS_PROP_LINKS | KAL_FS_PROP_MAKE_LINKS
                 | KAL_FS_PROP_EXECUTABLE;

        // Read-only formats: the nodes are there and none can be made, and a
        // rename cannot be atomic because there is no rename.
        case okl::fs_squashfs: case okl::fs_erofs:
            return KAL_FS_PROP_MODIFIED_TIME | KAL_FS_PROP_CASE_SENSITIVE
                 | KAL_FS_PROP_LINKS | KAL_FS_PROP_LOCKS | KAL_FS_PROP_CAPACITY
                 | KAL_FS_PROP_EXECUTABLE;
        case okl::fs_iso9660:
            return KAL_FS_PROP_MODIFIED_TIME | KAL_FS_PROP_CASE_SENSITIVE
                 | KAL_FS_PROP_LOCKS | KAL_FS_PROP_CAPACITY;

        // The FAT family stores neither a case distinction nor a node that
        // names another. `symlink' on such a volume reports EPERM, and this is
        // where a caller learns that before it tries.
        case okl::fs_msdos: case okl::fs_exfat:
            return conservative;

        // A case-insensitive namespace, with nodes that name others.
        case okl::fs_ntfs: case okl::fs_ntfs3:
            return conservative | KAL_FS_PROP_LINKS | KAL_FS_PROP_MAKE_LINKS;
        // The same, on a format that stores a mode.
        case okl::fs_hfsplus:
            return conservative | KAL_FS_PROP_LINKS | KAL_FS_PROP_MAKE_LINKS
                 | KAL_FS_PROP_EXECUTABLE;

        default:
            return conservative;
    }
}

// Writes no more of the structure than the caller says exists on its side, and
// reports which fields it filled.
//
// EVERY FIELD IS FILLED AND `wanted' IS IGNORED, WHICH IS THE ONE LINE THE
// SPECIFICATION SAYS THIS SHOULD BE. One `newfstatat' answers all of them on
// this kernel, so selecting would cost a branch and save nothing. An
// implementation whose environment answers them separately is the one `wanted'
// exists for.
//
// The exception is KAL_INFO_EXECUTABLE, which is a property of the volume as
// well as of the node: a format that stores no mode reports execute bits the
// mount chose, and reporting those would be answering for the volume. The
// volume is asked only when the caller wants the field, and only for a file.
void fill_info(const okl::kstat& st, kal_u32 wanted, kal_node_info* out,
               int volume_fd) {
    const kal_u32 self = out->self_size;
    kal_node_info v{};
    v.self_size   = self;
    v.present     = KAL_INFO_ALL & ~KAL_INFO_EXECUTABLE;
    v.size        = static_cast<kal_u64>(st.size);
    v.modified_ns = static_cast<kal_u64>(st.mtime_sec) * 1000000000u
                  + static_cast<kal_u64>(st.mtime_nsec);
    // The identity is the pair the kernel already keeps. It is opaque to a
    // caller, which may compare it and may not read it; using the device and
    // the inode is this kernel's answer and not the interface's shape.
    v.identity[0] = st.dev;
    v.identity[1] = st.ino;
    v.kind        = kind_of(st.mode);
    v.writable    = (st.mode & 0200u) != 0 ? 1 : 0;

    // A caller whose structure ends before the field is not given the position.
    const kal_u32 reaches = static_cast<kal_u32>(
        __builtin_offsetof(kal_node_info, executable) + sizeof v.executable);
    if ((wanted & KAL_INFO_EXECUTABLE) != 0 && self >= reaches
        && v.kind == kal_node_file
        && (volume_props(volume_fd) & KAL_FS_PROP_EXECUTABLE) != 0) {
        v.present   |= KAL_INFO_EXECUTABLE;
        v.executable = (st.mode & 0111u) != 0 ? 1 : 0;
    }

    const kal_u32 n = self < sizeof v ? self : (kal_u32)sizeof v;
    okl::copy(reinterpret_cast<char*>(out), reinterpret_cast<const char*>(&v), n);
}

// A node that refers to nothing, in the same shape.
void fill_absent(kal_node_info* out) {
    const kal_u32 self = out->self_size;
    kal_node_info v{};
    v.self_size = self;
    v.present   = KAL_INFO_KIND;
    v.kind      = kal_node_absent;
    const kal_u32 n = self < sizeof v ? self : (kal_u32)sizeof v;
    okl::copy(reinterpret_cast<char*>(out), reinterpret_cast<const char*>(&v), n);
}

// The caller must state how much of the structure exists on its side. A
// consumer that did not is a consumer whose structure this cannot be.
bool info_ok(const kal_node_info* out) {
    return out != nullptr && out->self_size >= sizeof(kal_u32) * 2;
}

// Copies a name into a caller's buffer and reports the length it HAS.
kal_uintptr put_name(const char* src, kal_uintptr n,
                     char* out, kal_uintptr cap, kal_uintptr* len) {
    if (out != nullptr && cap != 0) okl::copy(out, src, n < cap ? n : cap);
    if (len) *len = n;
    return n;
}

// Enumeration reads the kernel's own directory records. A C library's
// directory stream is not used, for the reason src/sys.h gives: the C library
// in a program above this one may be the one this implementation would be
// calling.
struct listing {
    int      fd;
    okl_uptr used;
    okl_uptr pos;
    char     buf[8192];
};

}  // namespace

extern "C" {

kal_uintptr kal_fs_preopen_count(void) {
    kal_uintptr n = 0; table(&n); return n;
}

int kal_fs_preopen(kal_uintptr index, kal_dir* out,
                   char* name_out, kal_uintptr name_cap, kal_uintptr* name_len) {
    kal_uintptr n = 0;
    preopen* t = table(&n);
    if (index >= n || out == nullptr) return kal_err_invalid;
    if (t[index].handle == 0) return kal_err_permission;
    *out = kal_dir{ t[index].handle };
    put_name(t[index].name, t[index].len, name_out, name_cap, name_len);
    return kal_ok;
}

int kal_fs_open_dir(kal_dir base, const char* name, kal_uintptr len, kal_dir* out) {
    const int b = okl::unpack(base.h);
    if (b < 0 || out == nullptr || !okl::acceptable(name, len)) return kal_err_invalid;
    okl::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    const okl_long fd = okl::sys(okl::nr_openat, b, reinterpret_cast<okl_long>(t.buf),
                                 okl::o_rdonly | okl::o_directory | okl::o_cloexec, 0);
    if (okl::failed(fd)) return okl::translate(fd);
    *out = kal_dir{ okl::pack(static_cast<int>(fd)) };
    return kal_ok;
}

int kal_fs_open(kal_dir base, const char* name, kal_uintptr len,
                kal_uintptr flags, kal_file* out) {
    const int b = okl::unpack(base.h);
    if (b < 0 || out == nullptr || !okl::acceptable(name, len)) return kal_err_invalid;
    okl::terminated t(name, len); if (!t.ok) return kal_err_invalid;

    const bool r = (flags & KAL_OPEN_READ)  != 0;
    const bool w = (flags & KAL_OPEN_WRITE) != 0;
    okl_long f = w ? (r ? okl::o_rdwr : okl::o_wronly) : okl::o_rdonly;
    f |= okl::o_cloexec;
    if (flags & KAL_OPEN_CREATE)    f |= okl::o_creat;
    if (flags & KAL_OPEN_EXCLUSIVE) f |= okl::o_excl;
    if (flags & KAL_OPEN_TRUNCATE)  f |= okl::o_trunc;
    if (flags & KAL_OPEN_APPEND)    f |= okl::o_append;

    const okl_long fd = okl::sys(okl::nr_openat, b, reinterpret_cast<okl_long>(t.buf), f, 0666);
    if (okl::failed(fd)) return okl::translate(fd);
    *out = kal_file{ okl::pack(static_cast<int>(fd)) };
    return kal_ok;
}

void kal_fs_close_dir(kal_dir d) {
    const int fd = okl::unpack(d.h);
    if (fd >= 0) { okl::retire(d.h); okl::sys(okl::nr_close, fd); }
}

void kal_fs_close_file(kal_file f) {
    const int fd = okl::unpack(f.h);
    if (fd >= 0) { okl::retire(f.h); okl::sys(okl::nr_close, fd); }
}

// A file's stream is the file. The descriptor is what openkal.stream's handle
// holds on this implementation, so no conversion is required and none is
// performed: the two interfaces agree because both are descriptor-shaped here,
// which is a property of this implementation and not of the specification.
kal_stream kal_fs_stream(kal_file f) {
    const int fd = okl::unpack(f.h);
    return kal_stream{ fd < 0 ? 0u : static_cast<kal_uintptr>(fd) };
}

// The greatest length of a name this implementation accepts.
kal_uintptr kal_fs_max_name(void) { return okl::max_name; }

int kal_fs_seek(kal_file f, kal_i64 offset, int whence, kal_u64* result) {
    const int fd = okl::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    int w = 0;
    if (whence == KAL_SEEK_CURRENT) w = 1;
    else if (whence == KAL_SEEK_END) w = 2;
    const okl_long r = okl::sys(okl::nr_lseek, fd, static_cast<okl_long>(offset), w);
    if (okl::failed(r)) return okl::translate(r);
    if (result) *result = static_cast<kal_u64>(r);
    return kal_ok;
}

int kal_fs_truncate(kal_file f, kal_u64 size) {
    const int fd = okl::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    const okl_long r = okl::sys(okl::nr_ftruncate, fd, static_cast<okl_long>(size));
    return okl::failed(r) ? okl::translate(r) : kal_ok;
}

int kal_fs_info(kal_dir base, const char* name, kal_uintptr len,
                kal_uintptr flags, kal_u32 wanted, kal_node_info* out) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !info_ok(out) || !okl::acceptable(name, len)) return kal_err_invalid;
    okl::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    okl::kstat st{};
    // RESOLVES BY DEFAULT, SO THAT ASKING AND OPENING ANSWER THE SAME QUESTION.
    //
    // This implementation asked with AT_SYMLINK_NOFOLLOW always while
    // `kal_fs_open' resolved, so a name referring to a node whose content is
    // another name was reported as that node while opening it reached a file.
    // A C library above reported a link where the host reports a regular file,
    // and one symbolic link made a whole tree uncopyable.
    const okl_long at = (flags & KAL_FS_NO_RESOLVE) ? okl::at_symlink_nofollow : 0;
    const okl_long r = okl::sys(okl::nr_newfstatat, b, reinterpret_cast<okl_long>(t.buf),
                                reinterpret_cast<okl_long>(&st), at);
    if (okl::failed(r)) {
        // Clause 7.7: enquiry about a name that does not exist is answered,
        // not refused. A component of the name that is not a directory is the
        // same answer, because the name still refers to nothing --- and so is a
        // node whose content names something absent, when the enquiry resolves.
        if (r == -okl::e_noent || r == -okl::e_notdir || r == -okl::e_loop) {
            fill_absent(out);
            return kal_ok;
        }
        return okl::translate(r);
    }
    fill_info(st, wanted, out, b);
    return kal_ok;
}

int kal_fs_file_info(kal_file f, kal_u32 wanted, kal_node_info* out) {
    const int fd = okl::unpack(f.h);
    if (fd < 0 || !info_ok(out)) return kal_err_invalid;
    okl::kstat st{};
    const okl_long r = okl::sys(okl::nr_fstat, fd, reinterpret_cast<okl_long>(&st));
    if (okl::failed(r)) return okl::translate(r);
    fill_info(st, wanted, out, fd);
    return kal_ok;
}

int kal_fs_set_modified(kal_file f, kal_u64 modified_ns) {
    const int fd = okl::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    // The kernel's operation takes two times and this interface names one, so
    // the other is given the value that means "leave it alone". Setting the
    // access time to the value openkal did not ask about would be an effect the
    // caller did not request and could not have predicted.
    constexpr okl_i64 utime_omit = 0x3ffffffe;
    okl::ktimespec times[2];
    times[0].sec = 0;  times[0].nsec = utime_omit;
    times[1].sec = static_cast<okl_i64>(modified_ns / 1000000000u);
    times[1].nsec = static_cast<okl_i64>(modified_ns % 1000000000u);
    // A null name and a descriptor is this kernel's spelling of "the open file
    // itself" rather than "a name beneath it".
    const okl_long r = okl::sys(okl::nr_utimensat, fd, 0,
                                reinterpret_cast<okl_long>(times), 0);
    return okl::failed(r) ? okl::translate(r) : kal_ok;
}

// The same, upon a NAME. Version 0.10.
//
// ADDED BECAUSE THE FORM ABOVE CANNOT REACH A DIRECTORY, AND A CONSUMER
// PAID FOR THAT. `kal_fs_set_modified' takes a `kal_file'; a directory is opened
// as a `kal_dir'; there was no third thing. openkal-musl reached a lock
// directory's timestamp by opening the directory for READING and setting the
// time on that, which worked here and was outside anything the interface said.
// This is the stated route.
int kal_fs_set_modified_at(kal_dir base, const char* name, kal_uintptr len,
                           kal_u64 modified_ns) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !okl::acceptable(name, len)) return kal_err_invalid;
    okl::terminated t(name, len); if (!t.ok) return kal_err_invalid;

    constexpr okl_i64 utime_omit = 0x3ffffffe;
    okl::ktimespec times[2];
    times[0].sec = 0;  times[0].nsec = utime_omit;
    times[1].sec  = static_cast<okl_i64>(modified_ns / 1000000000u);
    times[1].nsec = static_cast<okl_i64>(modified_ns % 1000000000u);

    // Resolves, because opening resolves and this is stated to agree with it.
    const okl_long r = okl::sys(okl::nr_utimensat, b,
                                reinterpret_cast<okl_long>(t.buf),
                                reinterpret_cast<okl_long>(times), 0);
    return okl::failed(r) ? okl::translate(r) : kal_ok;
}

// Whether a node may be started, version 0.13. This kernel stores the property
// as three bits, one per class of caller; the interface states one property,
// so a class that may read the node may start it, and clearing clears all
// three. Nothing else about the mode changes.
int kal_fs_set_executable_at(kal_dir base, const char* name, kal_uintptr len,
                             int executable) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !okl::acceptable(name, len)) return kal_err_invalid;
    okl::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    if ((volume_props(b) & KAL_FS_PROP_EXECUTABLE) == 0) return kal_err_not_supported;

    okl::kstat st{};
    okl_long r = okl::sys(okl::nr_newfstatat, b, reinterpret_cast<okl_long>(t.buf),
                          reinterpret_cast<okl_long>(&st), 0);
    if (okl::failed(r)) return okl::translate(r);
    const int k = kind_of(st.mode);
    if (k == kal_node_directory) return kal_err_is_directory;
    if (k != kal_node_file) return kal_err_invalid;

    const unsigned mode = static_cast<unsigned>(st.mode) & 07777u;
    const unsigned next = executable != 0 ? (mode | ((mode & 0444u) >> 2))
                                          : (mode & ~0111u);
    if (next == mode) return kal_ok;
    // Resolves, as the enquiry above did, so the node changed is the node read.
    r = okl::sys(okl::nr_fchmodat, b, reinterpret_cast<okl_long>(t.buf),
                 static_cast<okl_long>(next));
    return okl::failed(r) ? okl::translate(r) : kal_ok;
}

int kal_fs_mkdir(kal_dir base, const char* name, kal_uintptr len) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !okl::acceptable(name, len)) return kal_err_invalid;
    okl::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    const okl_long r = okl::sys(okl::nr_mkdirat, b, reinterpret_cast<okl_long>(t.buf), 0777);
    return okl::failed(r) ? okl::translate(r) : kal_ok;
}

int kal_fs_remove(kal_dir base, const char* name, kal_uintptr len) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !okl::acceptable(name, len)) return kal_err_invalid;
    okl::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    okl_long r = okl::sys(okl::nr_unlinkat, b, reinterpret_cast<okl_long>(t.buf), 0);
    if (!okl::failed(r)) return kal_ok;
    // One operation removes a name, and the kernel distinguishes two kinds of
    // name where this interface does not.
    if (r == -okl::e_isdir || r == -okl::e_perm) {
        const okl_long d = okl::sys(okl::nr_unlinkat, b, reinterpret_cast<okl_long>(t.buf),
                                    okl::at_removedir);
        if (!okl::failed(d)) return kal_ok;
        r = d;
    }
    return okl::translate(r);
}

int kal_fs_rename(kal_dir from, const char* a, kal_uintptr alen,
                  kal_dir to,   const char* b, kal_uintptr blen) {
    const int f = okl::unpack(from.h), t2 = okl::unpack(to.h);
    if (f < 0 || t2 < 0 || !okl::acceptable(a, alen) || !okl::acceptable(b, blen)) return kal_err_invalid;
    okl::terminated ta(a, alen), tb(b, blen);
    if (!ta.ok || !tb.ok) return kal_err_invalid;
    const okl_long r = okl::sys(okl::nr_renameat, f, reinterpret_cast<okl_long>(ta.buf),
                                t2, reinterpret_cast<okl_long>(tb.buf));
    return okl::failed(r) ? okl::translate(r) : kal_ok;
}

// Enumeration holds a descriptor of its own, obtained by opening the directory
// through itself. A duplicate would share the file offset with the handle the
// caller holds, so two enumerations of one directory would consume each
// other's entries.
int kal_fs_list_begin(kal_dir d, kal_uintptr* iter) {
    const int fd = okl::unpack(d.h);
    if (fd < 0 || iter == nullptr) return kal_err_invalid;
    const okl_long own = okl::sys(okl::nr_openat, fd, reinterpret_cast<okl_long>("."),
                                  okl::o_rdonly | okl::o_directory | okl::o_cloexec, 0);
    if (okl::failed(own)) return okl::translate(own);
    auto* s = static_cast<listing*>(kal_alloc(sizeof(listing), alignof(listing)));
    if (s == nullptr) { okl::sys(okl::nr_close, own); return kal_err_no_memory; }
    s->fd = static_cast<int>(own); s->used = 0; s->pos = 0;
    *iter = reinterpret_cast<kal_uintptr>(s);
    return kal_ok;
}

int kal_fs_list_next(kal_dir, kal_uintptr* iter,
                     char* name_out, kal_uintptr name_cap,
                     kal_uintptr* name_len, int* kind) {
    if (iter == nullptr || *iter == 0) return kal_err_invalid;
    auto* s = reinterpret_cast<listing*>(*iter);
    for (;;) {
        if (s->pos >= s->used) {
            const okl_long r = okl::sys(okl::nr_getdents64, s->fd,
                                        reinterpret_cast<okl_long>(s->buf),
                                        static_cast<okl_long>(sizeof s->buf));
            if (okl::interrupted(r)) continue;
            if (okl::failed(r) || r == 0) {
                okl::sys(okl::nr_close, s->fd);
                kal_free(s, sizeof(listing), alignof(listing));
                *iter = 0;
                if (name_len) *name_len = 0;
                return okl::failed(r) ? okl::translate(r) : kal_ok;
            }
            s->used = static_cast<okl_uptr>(r);
            s->pos  = 0;
        }
        auto* e = reinterpret_cast<okl::kdirent64*>(s->buf + s->pos);
        s->pos += e->reclen;
        // The two entries that name the directory and its parent are omitted.
        // They exist to support ascent, which this interface does not offer.
        if (e->name[0] == '.' && (e->name[1] == '\0'
            || (e->name[1] == '.' && e->name[2] == '\0'))) continue;
        put_name(e->name, okl::length(e->name), name_out, name_cap, name_len);
        if (kind) *kind = e->type == okl::dt_dir ? kal_node_directory
                        : e->type == okl::dt_reg ? kal_node_file
                        : e->type == okl::dt_lnk ? kal_node_link : kal_node_other;
        return kal_ok;
    }
}

// The properties of the volume a directory is on.
//
// AN ENQUIRY TAKING THE RESOURCE, BECAUSE EVERY POSITION IS A PROPERTY OF THE
// FORMAT. Version 0.6 answered with one word per implementation and claimed
// case sensitivity in it unconditionally --- which is false on any machine that
// has a FAT volume mounted, and this implementation offers the whole filesystem
// as a preopen, so such a volume is reachable through it. It also never claimed
// links, while every operation here met them.
//
// What is claimed for a format this implementation does not recognise is the
// set that cannot be wrong: the kernel reports a modification time for every
// filesystem it mounts, and `renameat' within one directory is atomic by POSIX.
// Case sensitivity and links are claimed only where the format is known to have
// them.
kal_uintptr kal_fs_props(kal_dir d) {
    return volume_props(okl::unpack(d.h));
}

// --- exclusion upon a range of a file --------------------------------------
//
// THE OPEN-FILE FORM, AND THE DIFFERENCE IS THE WHOLE REASON THIS IS
// WORTH SPECIFYING.
//
// This kernel's oldest record lock is held by the PROCESS and is released as
// soon as that process closes ANY descriptor for the node --- so a library that
// opened one file twice destroyed its own lock, and two parts of one program
// could not exclude each other at all. openkal states that the holder is the
// `kal_file', which is exactly what `F_OFD_*' describes: the lock belongs to the
// open file description and ends when the last descriptor for it closes, and
// when the program ends however it ends.
//
// Releasing on death is the half a caller cannot build for itself. Exclusion
// it can: `KAL_OPEN_EXCLUSIVE' and a name beside the file. What nothing above
// this line can do is release that name when its holder dies, so a program that
// ended abnormally while holding one would be locked out of its own file for
// ever.
static int lock_range(kal_file f, kal_u64 start, kal_u64 len,
                      short type, bool wait) {
    const int fd = okl::unpack(f.h);
    if (fd < 0) return kal_err_invalid;

    okl::kflock fl{};
    fl.l_type   = type;
    fl.l_whence = okl::seek_set;
    fl.l_start  = static_cast<okl_i64>(start);
    // openkal spells "to the end, however far that comes to be" as zero, and so
    // does this kernel. The two agree, so nothing is translated.
    fl.l_len    = static_cast<okl_i64>(len);

    const okl_long cmd = wait ? okl::f_ofd_setlkw : okl::f_ofd_setlk;
    okl_long r;
    do {
        r = okl::sys(okl::nr_fcntl, fd, cmd, reinterpret_cast<okl_long>(&fl));
    } while (okl::interrupted(r));
    if (!okl::failed(r)) return kal_ok;

    // TWO VALUES MEAN ONE THING HERE, AND openkal NAMES ONE OF THEM.
    //
    // The standard this call comes from says a range another holder has is
    // reported as EITHER of two values, and leaves the choice to the system ---
    // so a caller must accept both and an implementation of openkal must not
    // pass that choice on. openkal says `kal_err_again', which is the answer a
    // caller polls upon; the other value translates to `permission', which a
    // caller reads as "asking again will not help" and acts upon by stopping.
    //
    // Narrowed to the attempt that does NOT wait, because that is the only
    // one for which the two values carry this meaning. A permission failure on
    // any other path keeps its own answer.
    if (!wait && (-r == okl::e_acces || -r == okl::e_again)) return kal_err_again;
    return okl::translate(r);
}

int kal_fs_lock(kal_file f, kal_u64 start, kal_u64 len, kal_uintptr mode) {
    const bool shared    = (mode & KAL_LOCK_SHARED)    != 0;
    const bool exclusive = (mode & KAL_LOCK_EXCLUSIVE) != 0;
    // One of the two, and not both and not neither: a caller that asked for
    // both asked for something no environment has, and one that asked for
    // neither did not say what it wanted.
    if (shared == exclusive) return kal_err_invalid;
    return lock_range(f, start, len,
                      shared ? okl::lock_read : okl::lock_write,
                      (mode & KAL_LOCK_WAIT) != 0);
}

int kal_fs_unlock(kal_file f, kal_u64 start, kal_u64 len) {
    // Never waits: releasing is not a request another holder can block.
    return lock_range(f, start, len, okl::lock_unlock, false);
}

// --- how much the volume holds ----------------------------------------------
int kal_fs_capacity(kal_dir d, kal_u64* total, kal_u64* available) {
    const int fd = okl::unpack(d.h);
    if (fd < 0) return kal_err_invalid;

    okl::kstatfs sf{};
    const okl_long r = okl::sys(okl::nr_fstatfs, fd, reinterpret_cast<okl_long>(&sf));
    if (okl::failed(r)) return okl::translate(r);

    // In bytes, because that is what the interface says and what a caller of it
    // wants; this kernel reports blocks and the size of one.
    const kal_u64 unit = static_cast<kal_u64>(sf.f_bsize);
    // `f_bavail' AND NOT `f_bfree'. The second counts blocks the volume has,
    // including those only a privileged writer may reach; the first counts the
    // ones THIS program could actually use, which is the question asked.
    if (total)     *total     = static_cast<kal_u64>(sf.f_blocks) * unit;
    if (available) *available = static_cast<kal_u64>(sf.f_bavail) * unit;
    return kal_ok;
}

// Nodes whose content is another name.
int kal_fs_link_create(kal_dir base, const char* name, kal_uintptr len,
                       const char* target, kal_uintptr target_len,
                       kal_uintptr flags) {
    // The target is not a name this interface resolves: it is content, stored
    // and interpreted by whoever follows it later. It is therefore not passed
    // through `acceptable', which would refuse a target that ascends --- and a
    // target that ascends is the ordinary case for a relative one.
    (void)flags;   // this kernel does not distinguish a link to a directory
    const int b = okl::unpack(base.h);
    if (b < 0 || !okl::acceptable(name, len) || target == nullptr) return kal_err_invalid;
    okl::terminated n(name, len);        if (!n.ok) return kal_err_invalid;
    okl::terminated tgt(target, target_len); if (!tgt.ok) return kal_err_invalid;
    const okl_long r = okl::sys(okl::nr_symlinkat, reinterpret_cast<okl_long>(tgt.buf),
                                b, reinterpret_cast<okl_long>(n.buf));
    return okl::failed(r) ? okl::translate(r) : kal_ok;
}

kal_intptr kal_fs_link_read(kal_dir base, const char* name, kal_uintptr len,
                            char* out, kal_uintptr cap) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !okl::acceptable(name, len)) return -kal_err_invalid;
    okl::terminated t(name, len); if (!t.ok) return -kal_err_invalid;

    // The kernel truncates into the buffer it is given and does not report the
    // length the content has, so a caller asking for the length --- a capacity
    // of zero --- is served from a buffer of this implementation's own.
    char own[okl::max_name + 1];
    char* dst = (out != nullptr && cap != 0) ? out : own;
    okl_uptr room = (out != nullptr && cap != 0) ? cap : sizeof own;
    okl_long r = okl::sys(okl::nr_readlinkat, b, reinterpret_cast<okl_long>(t.buf),
                          reinterpret_cast<okl_long>(dst), static_cast<okl_long>(room));
    if (okl::failed(r)) return -okl::translate(r);

    // A result equal to the room given may have been truncated. Asking again
    // with room of this implementation's own is what turns "at least this" into
    // "this", and it is the only way this kernel offers.
    if (static_cast<okl_uptr>(r) == room && room < sizeof own) {
        const okl_long full = okl::sys(okl::nr_readlinkat, b,
                                       reinterpret_cast<okl_long>(t.buf),
                                       reinterpret_cast<okl_long>(own),
                                       static_cast<okl_long>(sizeof own));
        if (!okl::failed(full)) r = full;
    }
    return static_cast<kal_intptr>(r);
}

}
