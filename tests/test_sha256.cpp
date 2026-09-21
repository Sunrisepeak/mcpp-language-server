// SHA-256 against known digests (usable plan W9.4: payload integrity needs a trustworthy one).
import std;
import mcppls.testing;
import mcppls.base.sha256;

namespace b = mcppls::base;

int main() {
    using namespace mcppls::testing;

    "known digests"_test = [] {
        expect(b::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        expect(b::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        expect(b::sha256_hex("The quick brown fox jumps over the lazy dog")
               == "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    };

    "block-boundary lengths"_test = [] {
        // 56 is where padding needs a whole extra block; 64 is the block size itself: one byte on
        // either side, and the size itself, cover the paths process_block_ and finish() take.
        expect(b::sha256_hex(std::string(63, 'a')) == "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
        expect(b::sha256_hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
        expect(b::sha256_hex(std::string(65, 'a')) == "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
    };

    "streamed in arbitrary chunks matches one call"_test = [] {
        const std::string data(1000, 'x');
        const std::string whole { b::sha256_hex(data) };
        b::Sha256 streamed;
        std::size_t at { 0 };
        for (const std::size_t chunk : { 1u, 7u, 64u, 200u, 1u }) {
            const std::size_t take { std::min(chunk, data.size() - at) };
            streamed.update(std::string_view { data }.substr(at, take));
            at += take;
        }
        streamed.update(std::string_view { data }.substr(at));
        expect(streamed.finish() == whole);
        expect(whole == "44f8354494a5ba03ba1792a8d3e9c534c47a9181980fde7a3f44b06ef2ae7c7f");
    };

    return report();
}
