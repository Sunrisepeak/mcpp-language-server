// A zip archive written in memory (issue #23 fix plan F18): what a diagnostic bundle is packed in,
// since every system opens one without installing anything. The server carries no archive library
// (mcpp.toml keeps libarchive to packaging), so this is the part of the format a bundle needs:
// entries stored or deflated (RFC 1951, fixed Huffman codes), CRC-32, UTF-8 names, no ZIP64 (a
// bundle is capped far below 4 GiB).
export module mcppls.bundle.zip;

import std;

export namespace mcppls::bundle {

std::uint32_t crc32(std::string_view data);

// Raw deflate (no zlib or gzip header): LZ77 over a 32 KiB window, one block with the fixed codes.
std::string deflate(std::string_view data);

// One file of an archive, compressed already, so that a caller can decide which entries fit a size
// before any archive is written.
struct ZipEntry {
    std::string name;             // '/'-separated, relative
    std::uint32_t crc { 0 };
    std::uint64_t size { 0 };     // uncompressed
    std::uint16_t method { 0 };   // 0 stored, 8 deflated
    std::string data;             // as stored in the archive
};

// Deflated when that is smaller, stored otherwise.
ZipEntry make_entry(std::string name, std::string_view content);

// How many bytes an entry takes in an archive: its data and both of its headers.
std::uint64_t archive_size(const ZipEntry& entry);

// The archive, every entry dated `modified` (local time as the format has it; UTC here).
std::string zip_archive(std::span<const ZipEntry> entries, std::chrono::system_clock::time_point modified);

// An archive as zip_archive writes it, back to name -> content, each entry's size and CRC checked:
// for the checks that a bundle holds what its manifest says. Stored entries and deflate's stored
// and fixed-code blocks are read; an archive another program compressed with dynamic codes is not.
std::expected<std::map<std::string, std::string>, std::string> read_archive(std::string_view archive);

} // namespace mcppls::bundle
