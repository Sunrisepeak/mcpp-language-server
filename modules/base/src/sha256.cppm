// SHA-256 (FIPS 180-4). Payload integrity (usable plan W9.4) needs a digest of a couple of known
// files; that does not justify a cryptography dependency, so this is a small, from-scratch,
// streaming implementation instead.
export module mcppls.base.sha256;

import std;

export namespace mcppls::base {

class Sha256 {
public:
    Sha256();
    // May be called any number of times with chunks of any size.
    void update(std::string_view data);
    // The lowercase hex digest. Only valid to call once; the object is spent afterward.
    std::string finish();

private:
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> block_ {};
    std::size_t blockLength_ { 0 };
    std::uint64_t totalLength_ { 0 };   // bytes seen, for the trailing bit-length field only

    void process_block_();
};

// One-shot convenience for data that already fits in memory.
std::string sha256_hex(std::string_view data);

} // namespace mcppls::base
