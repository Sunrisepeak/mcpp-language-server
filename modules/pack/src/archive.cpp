module;

#include <archive.h>
#include <archive_entry.h>

module mcppls.pack.archive;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.platform.fs;

namespace mcppls::pack::archive {
namespace fs = mcppls::platform::fs;
namespace {

constexpr std::size_t READ_BLOCK { 1u << 20 };

// libarchive owns the handle; every exit from a read has to give it back.
struct Reader {
    ::archive* handle { nullptr };

    explicit Reader(std::string_view path) {
        handle = ::archive_read_new();
        if (handle == nullptr) return;
        ::archive_read_support_filter_all(handle);
        ::archive_read_support_format_all(handle);
        if (::archive_read_open_filename(handle, std::string { path }.c_str(), READ_BLOCK) != ARCHIVE_OK) {
            ::archive_read_free(handle);
            handle = nullptr;
        }
    }
    ~Reader() {
        if (handle != nullptr) {
            ::archive_read_close(handle);
            ::archive_read_free(handle);
        }
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
};

Kind kind_of(::archive_entry* entry) {
    if (::archive_entry_hardlink(entry) != nullptr) return Kind::hardlink;
    switch (::archive_entry_filetype(entry)) {
        case AE_IFDIR: return Kind::directory;
        case AE_IFLNK: return Kind::symlink;
        case AE_IFREG: return Kind::file;
        default:       return Kind::other;
    }
}

Entry entry_of(::archive_entry* raw) {
    Entry entry {};
    const char* name { ::archive_entry_pathname(raw) };
    entry.path = name != nullptr ? name : "";
    entry.kind = kind_of(raw);
    if (entry.kind == Kind::symlink) {
        const char* target { ::archive_entry_symlink(raw) };
        entry.linkTarget = target != nullptr ? target : "";
    } else if (entry.kind == Kind::hardlink) {
        const char* target { ::archive_entry_hardlink(raw) };
        entry.linkTarget = target != nullptr ? target : "";
    }
    entry.size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, ::archive_entry_size(raw)));
    entry.executable = (::archive_entry_perm(raw) & 0111) != 0;
    return entry;
}

// Splits on both separators. An archive written on Windows may use `\\`, and a name that
// reaches the file system of a Windows host is split there on either one, so a check that
// looked only at `/` would pass `a\\..\\..\\x` and the host would climb out anyway.
std::vector<std::string_view> components(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t start { 0 };
    for (std::size_t i { 0 }; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            if (i > start) parts.push_back(path.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

// The path this entry writes to, relative to the output directory, or nothing when it must not be
// written: absolute, a drive (`C:`) or any other name with a colon, or climbing out with `..`.
//
// `..` is resolved rather than refused on sight: `bin/../lib/x` stays inside and names `lib/x`.
// Only a `..` with nothing left to climb out of is an escape. The answer is the same on every
// host, so an archive that extracts on Linux extracts identically on Windows or not at all.
std::optional<std::string> safe_relative(std::string_view path) {
    if (path.starts_with('/') || path.starts_with('\\')) return std::nullopt;
    std::vector<std::string_view> kept;
    for (const auto part : components(path)) {
        if (part == ".") continue;
        if (part.contains(':')) return std::nullopt;
        if (part == "..") {
            if (kept.empty()) return std::nullopt;
            kept.pop_back();
            continue;
        }
        kept.push_back(part);
    }
    if (kept.empty()) return std::nullopt;
    std::string joined;
    for (const auto part : kept) {
        if (!joined.empty()) joined += '/';
        joined += part;
    }
    return joined;
}

// `./top/file` and `top/file` are the same member; some tar writers add the prefix.
std::string_view without_dot_slash(std::string_view path) {
    while (path.starts_with("./")) path.remove_prefix(2);
    return path;
}

// The archive's single top-level directory name, or an error saying what it found instead.
//
// A top-level FILE counts as a second root. Stripping would otherwise drop it without a word,
// which is the guess this function exists to refuse.
base::Result<std::string> sole_top_level(const std::vector<Entry>& entries) {
    std::set<std::string> roots;
    for (const auto& entry : entries) {
        const auto path = without_dot_slash(entry.path);
        if (path.empty() || path == ".") continue;
        const auto slash = path.find('/');
        roots.insert(std::string { slash == std::string_view::npos ? path : path.substr(0, slash) });
    }
    if (roots.size() != 1) {
        std::string found;
        for (const auto& root : roots) {
            if (!found.empty()) found += ", ";
            found += root;
        }
        return base::fail("archive-shape",
                          std::format("expected one top-level directory, found {}",
                                      roots.empty() ? std::string { "none" } : found));
    }
    return *roots.begin();
}

std::string_view strip_prefix(std::string_view path, std::string_view root) {
    if (root.empty()) return path;
    if (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/') {
        return path.substr(root.size() + 1);
    }
    return {};
}

base::Result<void> write_entry(::archive* reader, const std::string& destination) {
    if (auto made = fs::create_directories(base::parent_path(destination)); !made) {
        return std::unexpected { made.error() };
    }
    std::ofstream out { destination, std::ios::binary };
    if (!out) return base::fail("archive-write", std::format("cannot write {}", destination));

    const void* block { nullptr };
    std::size_t size { 0 };
    std::int64_t offset { 0 };
    while (true) {
        const int status = ::archive_read_data_block(reader, &block, &size, &offset);
        if (status == ARCHIVE_EOF) break;
        if (status < ARCHIVE_OK) {
            const char* why { ::archive_error_string(reader) };
            return base::fail("archive-read",
                              std::format("cannot read {}: {}", destination, why != nullptr ? why : "unknown"));
        }
        out.write(static_cast<const char*>(block), static_cast<std::streamsize>(size));
        if (!out) return base::fail("archive-write", std::format("cannot write {}", destination));
    }
    out.close();
    return {};
}

} // namespace

base::Result<std::vector<Entry>> list(std::string_view archivePath) {
    Reader reader { archivePath };
    if (reader.handle == nullptr) {
        return base::fail("archive-open", std::format("cannot open {}", archivePath));
    }

    std::vector<Entry> entries;
    ::archive_entry* raw { nullptr };
    while (true) {
        const int status = ::archive_read_next_header(reader.handle, &raw);
        if (status == ARCHIVE_EOF) break;
        if (status < ARCHIVE_OK) {
            const char* why { ::archive_error_string(reader.handle) };
            return base::fail("archive-read",
                              std::format("cannot read {}: {}", archivePath, why != nullptr ? why : "unknown"));
        }
        entries.push_back(entry_of(raw));
    }
    return entries;
}

base::Result<std::vector<std::string>> extract(std::string_view archivePath, std::string_view outDir,
                                               const std::function<bool(std::string_view)>& select,
                                               Options options) {
    auto entries = list(archivePath);
    if (!entries) return std::unexpected { entries.error() };

    std::string root;
    if (options.stripTopLevel) {
        auto sole = sole_top_level(*entries);
        if (!sole) return std::unexpected { sole.error() };
        root = *sole;
    }

    // The relative path each archive entry maps to, so the second pass does not redo this and the
    // link pass can ask whether a target was among the files actually written.
    std::map<std::string, std::string> relativeOf;   // archive path -> relative path
    for (const auto& entry : *entries) {
        const auto stripped = strip_prefix(without_dot_slash(entry.path), root);
        if (stripped.empty()) continue;
        if (auto safe = safe_relative(stripped)) relativeOf.emplace(entry.path, *safe);
    }

    std::vector<std::string> written;
    std::set<std::string> writtenSet;
    // Collected and set in one pass at the end: see fs::make_executable for why this costs a
    // process at all.
    std::vector<std::string> executables;
    // (relative path of the link, relative path it resolves to)
    std::vector<std::pair<std::string, std::string>> links;

    Reader reader { archivePath };
    if (reader.handle == nullptr) {
        return base::fail("archive-open", std::format("cannot open {}", archivePath));
    }

    ::archive_entry* raw { nullptr };
    while (true) {
        const int status = ::archive_read_next_header(reader.handle, &raw);
        if (status == ARCHIVE_EOF) break;
        if (status < ARCHIVE_OK) {
            const char* why { ::archive_error_string(reader.handle) };
            return base::fail("archive-read",
                              std::format("cannot read {}: {}", archivePath, why != nullptr ? why : "unknown"));
        }
        const Entry entry { entry_of(raw) };
        const auto found = relativeOf.find(entry.path);
        if (found == relativeOf.end()) continue;
        const std::string& relative { found->second };
        if (entry.kind == Kind::directory) continue;
        if (select && !select(relative)) continue;

        if (entry.kind == Kind::symlink || entry.kind == Kind::hardlink) {
            if (!options.materializeLinks) continue;
            // A symlink's target is relative to the link's own directory; a hard link names an
            // archive path outright.
            std::string target;
            if (entry.kind == Kind::symlink) {
                const std::string base { base::parent_path(relative) };
                target = base.empty() ? entry.linkTarget : base + "/" + entry.linkTarget;
            } else {
                const auto linked = relativeOf.find(entry.linkTarget);
                if (linked == relativeOf.end()) continue;
                target = linked->second;
            }
            if (auto normalized = safe_relative(target)) links.emplace_back(relative, *normalized);
            continue;
        }
        if (entry.kind != Kind::file) continue;

        const std::string destination { base::join_path(outDir, relative) };
        if (auto ok = write_entry(reader.handle, destination); !ok) {
            return std::unexpected { ok.error() };
        }
        written.push_back(relative);
        writtenSet.insert(relative);
        if (entry.executable) executables.push_back(destination);
    }

    // Links last: a link may appear before the file it points at, and only now is it known which
    // targets were written at all. Repeated until nothing changes, because a link may point at
    // another link (`libc++.so -> libc++.so.1 -> libc++.so.1.0`): each pass settles the links whose
    // target is now written, and a cycle or a chain that leaves the selection simply stops.
    std::vector<bool> settled(links.size(), false);
    for (bool progressed { true }; progressed;) {
        progressed = false;
        for (std::size_t i { 0 }; i < links.size(); ++i) {
            if (settled[i]) continue;
            const auto& [linkPath, target] = links[i];
            if (!writtenSet.contains(target)) continue;   // outside the selection, or not yet written
            const std::string from { base::join_path(outDir, target) };
            const std::string to { base::join_path(outDir, linkPath) };
            if (auto made = fs::create_directories(base::parent_path(to)); !made) {
                return std::unexpected { made.error() };
            }
            std::error_code failed;
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, failed);
            if (failed) {
                return base::fail("archive-link",
                                  std::format("cannot copy {} to {}: {}", target, linkPath, failed.message()));
            }
            written.push_back(linkPath);
            writtenSet.insert(linkPath);
            // A copy of an executable is an executable.
            if (std::ranges::find(executables, from) != executables.end()) executables.push_back(to);
            settled[i] = true;
            progressed = true;
        }
    }

    if (auto marked = fs::make_executable(executables); !marked) {
        return std::unexpected { marked.error() };
    }
    return written;
}

namespace {

// Streamed rather than loaded whole: a release's own server binary is tens of megabytes, and this
// writer exists so that stops being a reason to keep an interpreter around, not a reason to double
// it in memory.
base::Result<void> write_file_entry(::archive* out, const std::string& diskPath, const std::string& archiveName,
                                    bool executable) {
    const auto stamp = fs::stamp(diskPath);
    if (!stamp) return base::fail("archive-write", std::format("cannot stat {}", diskPath));
    std::ifstream in { diskPath, std::ios::binary };
    if (!in) return base::fail("archive-write", std::format("cannot read {}", diskPath));

    ::archive_entry* entry { ::archive_entry_new() };
    ::archive_entry_set_pathname(entry, archiveName.c_str());
    ::archive_entry_set_filetype(entry, AE_IFREG);
    ::archive_entry_set_size(entry, static_cast<std::int64_t>(stamp->size));
    ::archive_entry_set_perm(entry, executable ? 0755 : 0644);
    const bool headerOk { ::archive_write_header(out, entry) == ARCHIVE_OK };
    ::archive_entry_free(entry);
    if (!headerOk) {
        const char* why { ::archive_error_string(out) };
        return base::fail("archive-write",
                          std::format("cannot write header for {}: {}", archiveName, why != nullptr ? why : "unknown"));
    }

    std::string block(READ_BLOCK, '\0');
    while (in) {
        in.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        const auto wrote = ::archive_write_data(out, block.data(), got);
        if (wrote < 0 || static_cast<std::size_t>(wrote) != got) {
            const char* why { ::archive_error_string(out) };
            return base::fail("archive-write",
                              std::format("cannot write data for {}: {}", archiveName, why != nullptr ? why : "unknown"));
        }
    }
    if (in.bad()) return base::fail("archive-write", std::format("cannot read {}", diskPath));
    return {};
}

base::Result<void> write_directory_entry(::archive* out, const std::string& archiveName) {
    ::archive_entry* entry { ::archive_entry_new() };
    ::archive_entry_set_pathname(entry, (archiveName + "/").c_str());
    ::archive_entry_set_filetype(entry, AE_IFDIR);
    ::archive_entry_set_perm(entry, 0755);
    ::archive_entry_set_size(entry, 0);
    const bool ok { ::archive_write_header(out, entry) == ARCHIVE_OK };
    ::archive_entry_free(entry);
    if (!ok) {
        const char* why { ::archive_error_string(out) };
        return base::fail("archive-write",
                          std::format("cannot write directory {}: {}", archiveName, why != nullptr ? why : "unknown"));
    }
    return {};
}

// Depth-first over `mcppls.platform.fs::list_directory`'s already-sorted children, so the archive
// this writes is deterministic without a second sort here.
base::Result<void> add_recursive(::archive* out, const std::string& diskPath, const std::string& archiveName) {
    if (fs::is_directory(diskPath)) {
        if (auto wrote = write_directory_entry(out, archiveName); !wrote) return wrote;
        // `list_directory` already returns each child joined onto `diskPath`; the archive name
        // below it takes only the child's own file name, not that disk path.
        for (const auto& child : fs::list_directory(diskPath)) {
            if (auto added = add_recursive(out, child, archiveName + "/" + std::string { base::file_name(child) }); !added) {
                return added;
            }
        }
        return {};
    }
    if (fs::is_regular_file(diskPath)) {
        const auto mode = std::filesystem::status(diskPath).permissions();
        const bool executable { (mode & std::filesystem::perms::owner_exec) != std::filesystem::perms::none };
        return write_file_entry(out, diskPath, archiveName, executable);
    }
    return base::fail("archive-write", std::format("{} is neither a file nor a directory", diskPath));
}

} // namespace

base::Result<void> write(std::string_view archivePath, std::string_view rootName, std::span<const WriteEntry> entries) {
    ::archive* out { ::archive_write_new() };
    if (out == nullptr) return base::fail("archive-open", "archive_write_new failed");
    ::archive_write_set_format_pax_restricted(out);
    ::archive_write_add_filter_gzip(out);
    if (::archive_write_open_filename(out, std::string { archivePath }.c_str()) != ARCHIVE_OK) {
        const char* why { ::archive_error_string(out) };
        base::Error error { base::make_error(
            "archive-open", std::format("cannot open {} for writing: {}", archivePath, why != nullptr ? why : "unknown")) };
        ::archive_write_free(out);
        return std::unexpected { error };
    }

    base::Result<void> result {};
    for (const auto& entry : entries) {
        const std::string archiveName { entry.archivePath.empty() ? std::string { rootName }
                                                                   : std::string { rootName } + "/" + entry.archivePath };
        result = add_recursive(out, entry.source, archiveName);
        if (!result) break;
    }
    if (result && ::archive_write_close(out) != ARCHIVE_OK) {
        const char* why { ::archive_error_string(out) };
        result = base::fail("archive-write", std::format("cannot finish {}: {}", archivePath, why != nullptr ? why : "unknown"));
    }
    ::archive_write_free(out);
    return result;
}

} // namespace mcppls::pack::archive
