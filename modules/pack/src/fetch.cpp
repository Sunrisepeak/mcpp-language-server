module;

#include <cstdio>

module mcppls.pack.fetch;

import std;
import mcpplibs.tinyhttps;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.platform.fs;

namespace mcppls::pack::fetch {
namespace fs = mcppls::platform::fs;
namespace {

constexpr std::size_t BLOCK { 1u << 20 };

// A download of a toolchain is minutes of traffic, not seconds: the read timeout bounds one
// silent gap, not the transfer. Redirects are followed because the release hosts use them.
mcpplibs::tinyhttps::HttpClientConfig client_config() {
    mcpplibs::tinyhttps::HttpClientConfig config {};
    config.connectTimeoutMs = 30000;
    config.readTimeoutMs = 120000;
    config.maxRedirects = 10;
    return config;
}

std::string partial_name(std::string_view target) { return std::string { target } + ".partial"; }

void remove_quietly(const std::string& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

base::Result<std::string> digest_of(std::string_view path) {
    const std::string name { path };
    std::ifstream in { name, std::ios::binary };
    if (!in) return base::fail("fetch-read", std::format("cannot read {}", name));

    base::Sha256 digest {};
    std::string block(BLOCK, '\0');
    while (in) {
        in.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got > 0) digest.update(std::string_view { block.data(), got });
    }
    if (in.bad()) return base::fail("fetch-read", std::format("cannot read {}", name));
    return digest.finish();
}

base::Result<std::string> get(const Entry& entry, std::string_view cacheDir, int retries, bool quiet) {
    if (auto made = fs::create_directories(cacheDir); !made) return std::unexpected { made.error() };

    const std::string target { base::join_path(cacheDir, entry.file) };
    if (fs::is_regular_file(target)) {
        auto cached = digest_of(target);
        if (cached && *cached == entry.sha256) return fs::canonical_path(target);
        if (!quiet) {
            base::log::info("fetch: the cached {} does not match the lock; downloading it again", entry.file);
        }
        remove_quietly(target);
    }

    const std::string partial { partial_name(target) };
    mcpplibs::tinyhttps::HttpClient client { client_config() };
    std::string lastError { "no attempt was made" };

    for (int attempt = 1; attempt <= std::max(1, retries); ++attempt) {
        if (!quiet) base::log::info("fetch: {} (attempt {})", entry.url, attempt);
        remove_quietly(partial);

        const auto answer = client.download_to_file(entry.url, std::filesystem::path { partial });
        if (!answer.ok()) {
            lastError = answer.error.empty() ? std::format("HTTP {}", answer.statusCode) : answer.error;
        } else if (auto got = digest_of(partial); !got) {
            lastError = got.error().message;
        } else if (*got != entry.sha256) {
            lastError = std::format("sha256 mismatch for {}: the lock says {}, the download is {}",
                                    entry.file, entry.sha256, *got);
        } else if (entry.size && static_cast<std::uint64_t>(answer.bytesWritten) != *entry.size) {
            lastError = std::format("size mismatch for {}: the lock says {} bytes, the download is {}",
                                    entry.file, *entry.size, answer.bytesWritten);
        } else {
            std::error_code failed;
            std::filesystem::rename(partial, target, failed);
            if (!failed) return fs::canonical_path(target);
            lastError = std::format("cannot put the download at {}: {}", target, failed.message());
        }

        remove_quietly(partial);
        // Backs off, and stops backing off: a run that is going to fail should say so while someone
        // is still watching it.
        if (attempt < retries) {
            std::this_thread::sleep_for(std::chrono::seconds { std::min(30, 3 * attempt) });
        }
    }

    return base::fail("fetch", std::format("giving up on {}: {}", entry.file, lastError));
}

} // namespace mcppls::pack::fetch
