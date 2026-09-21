// Reading tar, tar.gz, tar.xz and zip through one reader.
//
// Packaging used to need two Python modules for this -- `tarfile` for the libc++ sources and
// `zipfile` for the clangd release -- which is most of why an interpreter was a build dependency
// at all. libarchive reads both, so the format is a property of the file rather than of the call
// site, and the same code serves every platform's inputs.
export module mcppls.pack.archive;

import std;
import mcppls.base.error;

export namespace mcppls::pack::archive {

enum class Kind { file, directory, symlink, hardlink, other };

struct Entry {
    std::string path;            // exactly as the archive records it
    Kind kind { Kind::file };
    std::string linkTarget;      // for symlink and hardlink
    std::uint64_t size { 0 };
    bool executable { false };   // any of the three execute bits
};

// Every entry's metadata, in archive order, writing nothing.
//
// Both callers need this before they can decide what to extract: the clangd trim has to find the
// one `lib/clang/<major>` the release happens to carry, and neither can be discovered halfway
// through writing files.
base::Result<std::vector<Entry>> list(std::string_view archivePath);

struct Options {
    // Drop the archive's single top-level directory so paths are relative to it. An archive with
    // more than one top-level entry is rejected rather than guessed at.
    bool stripTopLevel { true };

    // Write what a link points at instead of the link.
    //
    // The result is then identical on a host that cannot create links, and inside a VSIX, which
    // has no way to carry one. A link whose target is outside the selection is skipped rather
    // than followed onto the host filesystem.
    bool materializeLinks { true };
};

// Extracts the entries `select` accepts -- it sees the path as it will be written, after
// stripTopLevel -- into `outDir`. Returns the written paths, relative to outDir.
//
// An entry whose path is absolute or climbs out with `..` is refused: an archive is untrusted
// input, and these two are how it would write outside the directory it was given.
base::Result<std::vector<std::string>> extract(std::string_view archivePath, std::string_view outDir,
                                               const std::function<bool(std::string_view)>& select,
                                               Options options = {});

// One item to write into a new archive: a real file or directory on disk, and the path it lands at
// under the archive's single top-level directory (`rootName`). A directory is added recursively,
// depth-first in the sorted order `mcppls.platform.fs::list_directory` already gives, keeping the
// tree beneath it and each file's executable bit -- this is `xlings_artifacts.py`'s
// `write_archive`/`tarfile.add(..., recursive=True)`, the writing half of what used to need
// `tarfile`.
struct WriteEntry {
    std::string source;        // a file or a directory on disk
    std::string archivePath;   // where under `rootName/` it lands; empty writes at the root itself
};

// Writes a gzip-compressed tar whose every entry sits under one top-level directory `rootName`,
// mirroring what a payload's own tar.gz looks like once extracted. A symlink on disk is written as
// the file it resolves to: this is a WRITER for a release artifact, and a release must not carry a
// link the archive format, or the platform that unpacks it, would lose.
base::Result<void> write(std::string_view archivePath, std::string_view rootName, std::span<const WriteEntry> entries);

} // namespace mcppls::pack::archive
