module mcppls.bundle.zip;

import std;

namespace mcppls::bundle {

namespace {

constexpr std::array<std::uint32_t, 256> CRC_TABLE { [] {
    std::array<std::uint32_t, 256> table {};
    for (std::uint32_t n { 0 }; n < 256; ++n) {
        std::uint32_t c { n };
        for (int k { 0 }; k < 8; ++k) c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}() };

// Bits go out least significant first, as RFC 1951 packs them.
class BitWriter {
public:
    explicit BitWriter(std::string& out) : out_ { out } {}

    void write(std::uint32_t bits, int count) {
        buffer_ |= static_cast<std::uint64_t>(bits) << used_;
        used_ += count;
        while (used_ >= 8) {
            out_ += static_cast<char>(buffer_ & 0xFFU);
            buffer_ >>= 8;
            used_ -= 8;
        }
    }

    // A Huffman code, which the format writes most significant bit first.
    void write_code(std::uint32_t code, int length) {
        std::uint32_t reversed { 0 };
        for (int i { 0 }; i < length; ++i) reversed |= ((code >> i) & 1U) << (length - 1 - i);
        write(reversed, length);
    }

    void flush() {
        if (used_ > 0) out_ += static_cast<char>(buffer_ & 0xFFU);
        buffer_ = 0;
        used_ = 0;
    }

private:
    std::string& out_;
    std::uint64_t buffer_ { 0 };
    int used_ { 0 };
};

// The fixed literal/length code (RFC 1951 3.2.6).
void write_literal_length(BitWriter& bits, int symbol) {
    if (symbol <= 143) bits.write_code(0x30U + static_cast<std::uint32_t>(symbol), 8);
    else if (symbol <= 255) bits.write_code(0x190U + static_cast<std::uint32_t>(symbol - 144), 9);
    else if (symbol <= 279) bits.write_code(static_cast<std::uint32_t>(symbol - 256), 7);
    else bits.write_code(0xC0U + static_cast<std::uint32_t>(symbol - 280), 8);
}

constexpr std::array<int, 29> LENGTH_BASE { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr std::array<int, 29> LENGTH_EXTRA { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr std::array<int, 30> DISTANCE_BASE { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                              1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
constexpr std::array<int, 30> DISTANCE_EXTRA { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

void write_match(BitWriter& bits, int length, int distance) {
    std::size_t k { LENGTH_BASE.size() - 1 };
    while (LENGTH_BASE[k] > length) --k;
    write_literal_length(bits, 257 + static_cast<int>(k));
    if (LENGTH_EXTRA[k] > 0) bits.write(static_cast<std::uint32_t>(length - LENGTH_BASE[k]), LENGTH_EXTRA[k]);
    std::size_t d { DISTANCE_BASE.size() - 1 };
    while (DISTANCE_BASE[d] > distance) --d;
    bits.write_code(static_cast<std::uint32_t>(d), 5);
    if (DISTANCE_EXTRA[d] > 0) bits.write(static_cast<std::uint32_t>(distance - DISTANCE_BASE[d]), DISTANCE_EXTRA[d]);
}

void put16(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8) & 0xFFU);
}

void put32(std::string& out, std::uint32_t value) {
    put16(out, value & 0xFFFFU);
    put16(out, value >> 16);
}

// MS-DOS date and time, as the format stores a modification time.
std::pair<std::uint32_t, std::uint32_t> dos_date_time(std::chrono::system_clock::time_point when) {
    const auto days = std::chrono::floor<std::chrono::days>(when);
    const std::chrono::year_month_day date { days };
    const std::chrono::hh_mm_ss time { std::chrono::floor<std::chrono::seconds>(when - days) };
    const int year { std::clamp(static_cast<int>(date.year()), 1980, 2107) };
    const std::uint32_t dosDate { static_cast<std::uint32_t>(((year - 1980) << 9) | (static_cast<unsigned>(date.month()) << 5) | static_cast<unsigned>(date.day())) };
    const std::uint32_t dosTime { static_cast<std::uint32_t>((time.hours().count() << 11) | (time.minutes().count() << 5) | (time.seconds().count() / 2)) };
    return { dosDate, dosTime };
}

class BitReader {
public:
    explicit BitReader(std::string_view data) : data_ { data } {}
    std::optional<std::uint32_t> bits(int count) {
        std::uint32_t value { 0 };
        for (int i { 0 }; i < count; ++i) {
            const auto one = bit();
            if (!one) return std::nullopt;
            value |= *one << i;
        }
        return value;
    }
    std::optional<std::uint32_t> bit() {
        if (at_ / 8 >= data_.size()) return std::nullopt;
        const std::uint32_t value { (static_cast<unsigned char>(data_[at_ / 8]) >> (at_ % 8)) & 1U };
        ++at_;
        return value;
    }
    void align() { at_ = (at_ + 7) / 8 * 8; }
    std::size_t byte() const { return at_ / 8; }
    void skip_bytes(std::size_t n) { at_ += n * 8; }

private:
    std::string_view data_;
    std::size_t at_ { 0 };
};

std::optional<int> read_fixed_literal_length(BitReader& reader) {
    std::uint32_t code { 0 };
    for (int length { 1 }; length <= 9; ++length) {
        const auto bit = reader.bit();
        if (!bit) return std::nullopt;
        code = (code << 1) | *bit;
        if (length == 7 && code <= 23) return 256 + static_cast<int>(code);
        if (length == 8 && code >= 0x30 && code <= 0xBF) return static_cast<int>(code - 0x30);
        if (length == 8 && code >= 0xC0 && code <= 0xC7) return 280 + static_cast<int>(code - 0xC0);
        if (length == 9 && code >= 0x190 && code <= 0x1FF) return 144 + static_cast<int>(code - 0x190);
    }
    return std::nullopt;
}

std::expected<std::string, std::string> inflate(std::string_view data, std::uint64_t expected) {
    BitReader reader { data };
    std::string out;
    out.reserve(static_cast<std::size_t>(expected));
    bool last { false };
    while (!last) {
        const auto final = reader.bits(1);
        const auto type = reader.bits(2);
        if (!final || !type) return std::unexpected { std::string { "truncated deflate data" } };
        last = *final == 1;
        if (*type == 0) {
            reader.align();
            const std::size_t at { reader.byte() };
            if (at + 4 > data.size()) return std::unexpected { std::string { "truncated stored block" } };
            const std::size_t length { static_cast<unsigned char>(data[at]) | (static_cast<std::size_t>(static_cast<unsigned char>(data[at + 1])) << 8) };
            if (at + 4 + length > data.size()) return std::unexpected { std::string { "truncated stored block" } };
            out.append(data.substr(at + 4, length));
            reader.skip_bytes(4 + length);
            continue;
        }
        if (*type != 1) return std::unexpected { std::string { "a block with dynamic codes, which this reader does not read" } };
        while (true) {
            const auto symbol = read_fixed_literal_length(reader);
            if (!symbol) return std::unexpected { std::string { "a code that is not a fixed code" } };
            if (*symbol < 256) {
                out += static_cast<char>(*symbol);
                continue;
            }
            if (*symbol == 256) break;
            const std::size_t k { static_cast<std::size_t>(*symbol - 257) };
            if (k >= LENGTH_BASE.size()) return std::unexpected { std::string { "a length symbol out of range" } };
            const auto lengthExtra = reader.bits(LENGTH_EXTRA[k]);
            std::uint32_t code { 0 };
            for (int i { 0 }; i < 5; ++i) {
                const auto bit = reader.bit();
                if (!bit) return std::unexpected { std::string { "truncated distance" } };
                code = (code << 1) | *bit;
            }
            if (!lengthExtra || code >= DISTANCE_BASE.size()) return std::unexpected { std::string { "a distance code out of range" } };
            const auto distanceExtra = reader.bits(DISTANCE_EXTRA[code]);
            if (!distanceExtra) return std::unexpected { std::string { "truncated distance" } };
            const std::size_t length { static_cast<std::size_t>(LENGTH_BASE[k]) + *lengthExtra };
            const std::size_t distance { static_cast<std::size_t>(DISTANCE_BASE[code]) + *distanceExtra };
            if (distance > out.size()) return std::unexpected { std::string { "a distance before the start" } };
            const std::size_t from { out.size() - distance };
            for (std::size_t i { 0 }; i < length; ++i) out += out[from + i];
        }
    }
    return out;
}

std::uint32_t get16(std::string_view data, std::size_t at) {
    return static_cast<unsigned char>(data[at]) | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 1])) << 8);
}
std::uint32_t get32(std::string_view data, std::size_t at) { return get16(data, at) | (get16(data, at + 2) << 16); }

constexpr std::uint32_t UTF8_NAMES { 0x0800 };
constexpr std::uint32_t VERSION_NEEDED { 20 };
constexpr std::uint32_t MADE_BY_UNIX { (3U << 8) | 20U };
constexpr std::uint32_t REGULAR_FILE_0644 { 0100644U << 16 };

} // namespace

std::uint32_t crc32(std::string_view data) {
    std::uint32_t crc { 0xFFFFFFFFU };
    for (const char c : data) crc = CRC_TABLE[(crc ^ static_cast<unsigned char>(c)) & 0xFFU] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

std::string deflate(std::string_view data) {
    static constexpr int WINDOW { 32768 };
    static constexpr int HASH_BITS { 15 };
    static constexpr int MIN_MATCH { 3 };
    static constexpr int MAX_MATCH { 258 };
    static constexpr int MAX_CHAIN { 48 };
    std::string out;
    out.reserve(data.size() / 3 + 16);
    BitWriter bits { out };
    bits.write(1, 1);   // BFINAL: the only block
    bits.write(1, 2);   // BTYPE 01: fixed Huffman codes
    const auto n = static_cast<std::int64_t>(data.size());
    std::vector<std::int64_t> head(std::size_t { 1 } << HASH_BITS, -1);
    std::vector<std::int64_t> previous(WINDOW, -1);
    const auto byte = [&](std::int64_t at) { return static_cast<std::uint32_t>(static_cast<unsigned char>(data[static_cast<std::size_t>(at)])); };
    const auto hash = [&](std::int64_t at) {
        const std::uint32_t key { (byte(at) << 16) | (byte(at + 1) << 8) | byte(at + 2) };
        return static_cast<std::size_t>((key * 2654435761U) >> (32 - HASH_BITS));
    };
    const auto insert = [&](std::int64_t at) {
        if (at + MIN_MATCH > n) return;
        const std::size_t h { hash(at) };
        previous[static_cast<std::size_t>(at % WINDOW)] = head[h];
        head[h] = at;
    };
    std::int64_t i { 0 };
    while (i < n) {
        int bestLength { 0 };
        std::int64_t bestDistance { 0 };
        if (i + MIN_MATCH <= n) {
            std::int64_t candidate { head[hash(i)] };
            const int limit { static_cast<int>(std::min<std::int64_t>(MAX_MATCH, n - i)) };
            for (int chain { 0 }; candidate >= 0 && chain < MAX_CHAIN && i - candidate <= WINDOW; ++chain) {
                int length { 0 };
                while (length < limit && data[static_cast<std::size_t>(candidate + length)] == data[static_cast<std::size_t>(i + length)]) ++length;
                if (length > bestLength) {
                    bestLength = length;
                    bestDistance = i - candidate;
                    if (length == limit) break;
                }
                const std::int64_t next { previous[static_cast<std::size_t>(candidate % WINDOW)] };
                if (next >= candidate) break;   // a slot the window has since reused
                candidate = next;
            }
        }
        if (bestLength >= MIN_MATCH) {
            write_match(bits, bestLength, static_cast<int>(bestDistance));
            for (int k { 0 }; k < bestLength; ++k) insert(i + k);
            i += bestLength;
        } else {
            write_literal_length(bits, static_cast<int>(byte(i)));
            insert(i);
            ++i;
        }
    }
    write_literal_length(bits, 256);   // end of block
    bits.flush();
    return out;
}

ZipEntry make_entry(std::string name, std::string_view content) {
    ZipEntry entry;
    entry.name = std::move(name);
    entry.crc = crc32(content);
    entry.size = content.size();
    std::string deflated { deflate(content) };
    if (deflated.size() < content.size()) {
        entry.method = 8;
        entry.data = std::move(deflated);
    } else {
        entry.method = 0;
        entry.data = std::string { content };
    }
    return entry;
}

std::uint64_t archive_size(const ZipEntry& entry) { return entry.data.size() + 30 + 46 + 2 * entry.name.size(); }

std::string zip_archive(std::span<const ZipEntry> entries, std::chrono::system_clock::time_point modified) {
    const auto [date, time] = dos_date_time(modified);
    std::string out;
    std::string directory;
    std::uint64_t total { 22 };
    for (const auto& entry : entries) total += archive_size(entry);
    out.reserve(static_cast<std::size_t>(total));
    for (const auto& entry : entries) {
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto compressed = static_cast<std::uint32_t>(entry.data.size());
        const auto size = static_cast<std::uint32_t>(entry.size);
        const auto nameLength = static_cast<std::uint32_t>(entry.name.size());
        put32(out, 0x04034B50U);
        put16(out, VERSION_NEEDED);
        put16(out, UTF8_NAMES);
        put16(out, entry.method);
        put16(out, time);
        put16(out, date);
        put32(out, entry.crc);
        put32(out, compressed);
        put32(out, size);
        put16(out, nameLength);
        put16(out, 0);
        out += entry.name;
        out += entry.data;

        put32(directory, 0x02014B50U);
        put16(directory, MADE_BY_UNIX);
        put16(directory, VERSION_NEEDED);
        put16(directory, UTF8_NAMES);
        put16(directory, entry.method);
        put16(directory, time);
        put16(directory, date);
        put32(directory, entry.crc);
        put32(directory, compressed);
        put32(directory, size);
        put16(directory, nameLength);
        put16(directory, 0);   // extra field
        put16(directory, 0);   // comment
        put16(directory, 0);   // disk
        put16(directory, 0);   // internal attributes
        put32(directory, REGULAR_FILE_0644);
        put32(directory, offset);
        directory += entry.name;
    }
    const auto directoryOffset = static_cast<std::uint32_t>(out.size());
    out += directory;
    put32(out, 0x06054B50U);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, static_cast<std::uint32_t>(directory.size()));
    put32(out, directoryOffset);
    put16(out, 0);
    return out;
}

std::expected<std::map<std::string, std::string>, std::string> read_archive(std::string_view archive) {
    const std::size_t end { archive.rfind(std::string_view { "PK\x05\x06", 4 }) };
    if (end == std::string_view::npos || end + 22 > archive.size()) return std::unexpected { std::string { "no end of central directory" } };
    const std::uint32_t count { get16(archive, end + 10) };
    std::size_t at { get32(archive, end + 16) };
    std::map<std::string, std::string> files;
    for (std::uint32_t i { 0 }; i < count; ++i) {
        if (at + 46 > archive.size() || get32(archive, at) != 0x02014B50U) return std::unexpected { std::string { "a bad central directory entry" } };
        const std::uint32_t method { get16(archive, at + 10) };
        const std::uint32_t crc { get32(archive, at + 16) };
        const std::uint32_t compressed { get32(archive, at + 20) };
        const std::uint32_t size { get32(archive, at + 24) };
        const std::uint32_t nameLength { get16(archive, at + 28) };
        const std::uint32_t local { get32(archive, at + 42) };
        if (at + 46 + nameLength > archive.size()) return std::unexpected { std::string { "a name past the end" } };
        std::string name { archive.substr(at + 46, nameLength) };
        at += 46 + nameLength + get16(archive, at + 30) + get16(archive, at + 32);
        if (local + 30 > archive.size() || get32(archive, local) != 0x04034B50U) return std::unexpected { std::format("{}: a bad local header", name) };
        const std::size_t data { local + 30 + get16(archive, local + 26) + get16(archive, local + 28) };
        if (data + compressed > archive.size()) return std::unexpected { std::format("{}: data past the end", name) };
        std::string content;
        if (method == 0) {
            content = std::string { archive.substr(data, compressed) };
        } else if (method == 8) {
            auto inflated = inflate(archive.substr(data, compressed), size);
            if (!inflated) return std::unexpected { std::format("{}: {}", name, inflated.error()) };
            content = std::move(*inflated);
        } else {
            return std::unexpected { std::format("{}: compression method {}", name, method) };
        }
        if (content.size() != size || crc32(content) != crc) return std::unexpected { std::format("{}: its size or CRC is not what the archive says", name) };
        files.emplace(std::move(name), std::move(content));
    }
    return files;
}

} // namespace mcppls::bundle
