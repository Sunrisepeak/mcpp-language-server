module;

#include <cstdio>

module mcppls.pack.fetch;

import std;
import mcpplibs.tinyhttps;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.sha256;
import mcpplibs.os;
import mcppls.base.text;
import mcppls.toolrun;

namespace os = mcpplibs::os;
namespace mcppls::pack::fetch {
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

// GitHub's answer to a release download -- a redirect to its asset host -- carries a content-security-policy
// line of 9.2 KB (measured 2026-10-24); tinyhttps 0.3 gives up on any header line over 8 KB. curl, on every CI
// runner and developer machine, takes such a download instead, verified the same way. Remove when tinyhttps
// takes longer lines.
bool header_line_too_long(std::string_view error) { return error.contains("line exceeds"); }

// The download with curl, into `partial`; empty when it worked, else why not.
std::string download_with_curl(const std::string& url, const std::string& partial) {
    const auto curl = toolrun::find_program("curl");
    if (!curl) return "curl is not on PATH";
    os::process::spawn_options spawn;
    spawn.bound_lifetime = true;
    spawn.program = *curl;
    spawn.args = { "--fail", "--silent", "--show-error", "--location", "--connect-timeout", "30", "--output", partial, url };
    const auto ran = os::process::run(std::move(spawn), os::process::bounds { .hard = std::chrono::minutes { 30 } });
    if (!ran) return ran.error().message();
    if (ran->timed_out) return "curl did not finish in time";
    if (!ran->status.success()) return std::format("curl exited {}: {}", toolrun::exit_code(ran->status), base::last_lines(ran->err, 3));
    return {};
}

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
    std::error_code ignored;
    if (std::error_code failed; !std::filesystem::create_directories(cacheDir, failed) && failed) return base::fail("create-directory", std::format("cannot create {}: {}", cacheDir, failed.message()));

    const std::string target { base::join_path(cacheDir, entry.file) };
    if (std::filesystem::is_regular_file(target, ignored)) {
        auto cached = digest_of(target);
        if (cached && *cached == entry.sha256) return base::canonical_path(target);
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
        std::uint64_t written { static_cast<std::uint64_t>(answer.bytesWritten) };
        std::string failed;
        if (!answer.ok()) failed = answer.error.empty() ? std::format("HTTP {}", answer.statusCode) : answer.error;
        if (!failed.empty() && header_line_too_long(failed)) {
            if (!quiet) base::log::info("fetch: {} ({}); downloading it with curl", entry.url, failed);
            remove_quietly(partial);
            if (const auto why = download_with_curl(entry.url, partial); why.empty()) {
                failed.clear();
                written = base::file_stamp(partial) ? base::file_stamp(partial)->size : 0;
            } else {
                failed = std::format("{}; with curl: {}", failed, why);
            }
        }
        if (!failed.empty()) {
            lastError = failed;
        } else if (auto got = digest_of(partial); !got) {
            lastError = got.error().message;
        } else if (*got != entry.sha256) {
            lastError = std::format("sha256 mismatch for {}: the lock says {}, the download is {}",
                                    entry.file, entry.sha256, *got);
        } else if (entry.size && written != *entry.size) {
            lastError = std::format("size mismatch for {}: the lock says {} bytes, the download is {}",
                                    entry.file, *entry.size, written);
        } else {
            std::error_code failed;
            std::filesystem::rename(partial, target, failed);
            if (!failed) return base::canonical_path(target);
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
