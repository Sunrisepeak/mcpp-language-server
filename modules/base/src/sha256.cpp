module mcppls.base.sha256;

import std;

namespace mcppls::base {

namespace {

// The 64 round constants: fractional parts of the cube roots of the first 64 primes (FIPS 180-4).
constexpr std::array<std::uint32_t, 64> K { {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
} };

constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

} // namespace

// Fractional parts of the square roots of the first 8 primes (FIPS 180-4).
Sha256::Sha256()
    : state_ { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 } {}

void Sha256::process_block_() {
    std::array<std::uint32_t, 64> w {};
    for (int i { 0 }; i < 16; ++i) {
        w[i] = (std::uint32_t { block_[i * 4] } << 24) | (std::uint32_t { block_[i * 4 + 1] } << 16)
             | (std::uint32_t { block_[i * 4 + 2] } << 8) | std::uint32_t { block_[i * 4 + 3] };
    }
    for (int i { 16 }; i < 64; ++i) {
        const std::uint32_t s0 { rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3) };
        const std::uint32_t s1 { rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10) };
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a { state_[0] }, b { state_[1] }, c { state_[2] }, d { state_[3] };
    std::uint32_t e { state_[4] }, f { state_[5] }, g { state_[6] }, h { state_[7] };
    for (int i { 0 }; i < 64; ++i) {
        const std::uint32_t s1 { rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25) };
        const std::uint32_t ch { (e & f) ^ (~e & g) };
        const std::uint32_t temp1 { h + s1 + ch + K[i] + w[i] };
        const std::uint32_t s0 { rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22) };
        const std::uint32_t maj { (a & b) ^ (a & c) ^ (b & c) };
        const std::uint32_t temp2 { s0 + maj };
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::string_view data) {
    totalLength_ += data.size();
    std::size_t offset { 0 };
    while (offset < data.size()) {
        const std::size_t take { std::min(data.size() - offset, std::size_t { 64 } - blockLength_) };
        for (std::size_t i { 0 }; i < take; ++i) block_[blockLength_ + i] = static_cast<std::uint8_t>(data[offset + i]);
        blockLength_ += take;
        offset += take;
        if (blockLength_ == 64) {
            process_block_();
            blockLength_ = 0;
        }
    }
}

std::string Sha256::finish() {
    const std::uint64_t bitLength { totalLength_ * 8 };
    // Append the mandatory '1' bit (a whole 0x80 byte, since everything here is byte-aligned), zero
    // pad to a 56-byte boundary (spilling into an extra block first if there is no room left in
    // this one), then the 64-bit big-endian bit length.
    block_[blockLength_++] = 0x80;
    if (blockLength_ > 56) {
        while (blockLength_ < 64) block_[blockLength_++] = 0;
        process_block_();
        blockLength_ = 0;
    }
    while (blockLength_ < 56) block_[blockLength_++] = 0;
    for (int i { 0 }; i < 8; ++i) block_[56 + i] = static_cast<std::uint8_t>(bitLength >> (56 - 8 * i));
    process_block_();

    static constexpr std::string_view DIGITS { "0123456789abcdef" };
    std::string hex;
    hex.reserve(64);
    for (const auto word : state_) {
        for (int shift { 24 }; shift >= 0; shift -= 8) {
            const std::uint8_t byteValue { static_cast<std::uint8_t>(word >> shift) };
            hex.push_back(DIGITS[byteValue >> 4]);
            hex.push_back(DIGITS[byteValue & 0x0f]);
        }
    }
    return hex;
}

std::string sha256_hex(std::string_view data) {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

} // namespace mcppls::base
