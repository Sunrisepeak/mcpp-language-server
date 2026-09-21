module mcppls.platform.fs;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import openkal.fs;
import mcppls.platform.preopen;
import mcppls.os;

namespace mcppls::platform::fs {

namespace {

std::filesystem::path native(std::string_view path) { return std::filesystem::path { std::string { path } }; }

std::string from_native(const std::filesystem::path& path) { return base::normalize_path(path.generic_string()); }

std::atomic<std::uint64_t> gTemporaryCounter { 0 };

} // namespace

bool exists(std::string_view path) {
    std::error_code error;
    return std::filesystem::exists(native(path), error);
}

bool is_directory(std::string_view path) {
    std::error_code error;
    return std::filesystem::is_directory(native(path), error);
}

bool is_regular_file(std::string_view path) {
    std::error_code error;
    return std::filesystem::is_regular_file(native(path), error);
}

std::optional<FileStamp> stamp(std::string_view path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(native(path), error);
    if (error) return std::nullopt;
    const auto time = std::filesystem::last_write_time(native(path), error);
    if (error) return std::nullopt;
    return FileStamp { static_cast<std::uint64_t>(size),
                       static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count()) };
}

base::Result<std::string> read_file(std::string_view path) {
    std::ifstream stream { native(path), std::ios::binary };
    if (!stream) return base::fail("read-file", std::format("cannot open {}", path));
    std::string content { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    if (stream.bad()) return base::fail("read-file", std::format("cannot read {}", path));
    return content;
}

base::Result<void> write_file(std::string_view path, std::string_view content) {
    std::ofstream stream { native(path), std::ios::binary | std::ios::trunc };
    if (!stream) return base::fail("write-file", std::format("cannot create {}", path));
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
    if (!stream) return base::fail("write-file", std::format("cannot write {}", path));
    return {};
}

base::Result<void> write_file_atomic(std::string_view path, std::string_view content) {
    const std::string temporary { std::format("{}.tmp-{}-{}", path,
        std::chrono::steady_clock::now().time_since_epoch().count(), gTemporaryCounter.fetch_add(1)) };
    if (auto written = write_file(temporary, content); !written) return written;
    std::error_code error;
    std::filesystem::rename(native(temporary), native(path), error);
    if (error) {
        // Some systems refuse to replace an existing file by rename.
        std::filesystem::remove(native(path), error);
        error.clear();
        std::filesystem::rename(native(temporary), native(path), error);
    }
    if (error) {
        std::filesystem::remove(native(temporary), error);
        return base::fail("write-file", std::format("cannot replace {}", path));
    }
    return {};
}

base::Result<void> create_directories(std::string_view path) {
    if (is_directory(path)) return {};
    // Created one component at a time: a volume root has no parent to create.
    const std::string normalized { base::normalize_path(path) };
    std::string parent { base::parent_path(normalized) };
    if (!parent.empty() && parent != normalized && !is_directory(parent)) {
        if (auto created = create_directories(parent); !created) return created;
    }
    std::error_code error;
    std::filesystem::create_directory(native(normalized), error);
    if (error && !is_directory(normalized)) {
        return base::fail("create-directory", std::format("cannot create {}: {}", path, error.message()));
    }
    return {};
}

void remove_all(std::string_view path) {
    std::error_code error;
    std::filesystem::remove_all(native(path), error);
}

std::vector<std::string> list_files(std::string_view root, std::span<const std::string_view> extensions,
                                    std::span<const std::string_view> skipDirectories) {
    std::vector<std::string> result;
    std::vector<std::string> pending { base::normalize_path(root) };
    while (!pending.empty()) {
        std::string directory { std::move(pending.back()) };
        pending.pop_back();
        std::error_code error;
        std::filesystem::directory_iterator iterator { native(directory), error };
        if (error) continue;
        for (const auto& entry : iterator) {
            const std::string name { entry.path().filename().generic_string() };
            const std::string full { base::join_path(directory, name) };
            std::error_code statusError;
            if (entry.is_directory(statusError)) {
                if (name.starts_with('.')) continue;
                if (std::ranges::any_of(skipDirectories, [&](std::string_view skip) { return name == skip; })) continue;
                if (entry.is_symlink(statusError)) continue;
                pending.push_back(full);
            } else if (entry.is_regular_file(statusError)) {
                const std::string_view suffix { base::extension(name) };
                if (extensions.empty()
                    || std::ranges::any_of(extensions, [&](std::string_view ext) { return base::iequals_ascii(ext, suffix); })) {
                    result.push_back(full);
                }
            }
        }
    }
    std::ranges::sort(result);
    return result;
}

std::vector<std::string> list_directory(std::string_view path) {
    std::vector<std::string> result;
    std::error_code error;
    std::filesystem::directory_iterator iterator { native(path), error };
    if (error) return result;
    for (const auto& entry : iterator) {
        result.push_back(base::join_path(path, entry.path().filename().generic_string()));
    }
    std::ranges::sort(result);
    return result;
}

std::string current_directory() {
    std::error_code error;
    const auto path = std::filesystem::current_path(error);
    if (error) return {};
    return from_native(path);
}

std::optional<FileIdentity> file_identity(std::string_view path) {
    const auto resolved = resolve_name(path);
    if (!resolved || resolved->remainder.empty()) return std::nullopt;
    kal_node_info info { kal::fs::info_for_caller() };
    const std::string& name { resolved->remainder };
    if (kal_fs_info(resolved->directory, name.data(), name.size(), 0, kal::fs::field::identity, &info) != kal_ok) return std::nullopt;
    if ((info.present & kal::fs::field::identity) == 0) return std::nullopt;
    return FileIdentity { info.identity[0], info.identity[1] };
}

std::string canonical_path(std::string_view path) {
    if (path.empty()) return {};
    if constexpr (base::NATIVE_PATH_STYLE == base::PathStyle::windows) {
        // A short alias always carries a '~'. A component that does is looked up
        // among its parent's entries, which are listed by their long names, and
        // replaced by the one that is the same file.
        const std::string normalized { base::normalize_path(path) };
        if (!normalized.contains('~') || normalized.size() < 2 || normalized[1] != ':') return normalized;
        std::string current { normalized.substr(0, 2) };
        for (const auto part : base::split(std::string_view { normalized }.substr(2), '/')) {
            if (part.empty()) continue;
            std::string next { current + "/" + std::string { part } };
            if (part.contains('~')) {
                if (const auto identity = file_identity(next)) {
                    for (const auto& entry : list_directory(current.size() == 2 ? current + "/" : current)) {
                        if (base::file_name(entry).contains('~')) continue;
                        if (const auto other = file_identity(entry); other && *other == *identity) {
                            next = current + "/" + std::string { base::file_name(entry) };
                            break;
                        }
                    }
                }
            }
            current = std::move(next);
        }
        return current;
    } else {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(native(path), error);
        if (error || resolved.empty()) return base::normalize_path(path);
        return from_native(resolved);
    }
}

} // namespace mcppls::platform::fs

namespace mcppls::platform::fs {

base::Result<void> make_executable(std::span<const std::string> paths) {
    // WINDOWS HAS NO EXECUTE BIT, AND ASKING FOR ONE IS NOT A NO-OP THERE.
    //
    // What makes a file runnable on Windows is its extension, so there is
    // nothing to grant and nothing that could have been withheld. Asked
    // anyway, `std::filesystem::permissions` reports an error, and this
    // function then failed every extraction that contained one executable ---
    // `ar::extract` returned `unexpected`, `test_archive.cpp:109` saw an
    // empty result, and `:110` dereferenced it. The fault address was
    // 0x0000646f6d68630a, which is "chmod\n": the category of the error
    // object being read as a pointer.
    if constexpr (os::FAMILY == os::Family::windows) {
        (void)paths;
        return {};
    }
    for (const auto& path : paths) {
        std::error_code failed;
        // All three execute bits, always, and that is not tidiness.
        //
        // openkal-musl grants a chmod only for a mode it can honestly report back, and what it
        // reports is one read triple and one write triple, each all-set or all-clear (a capability
        // volume has no per-class mode word). Asking for owner_exec alone would produce 0744, which
        // is not a shape it can express, and it refuses with ENOSYS rather than rounding -- so the
        // narrower-looking request is the one that silently fails. 0666 | 0111 = 0777 is expressible.
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec
                                         | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, failed);
        if (failed) {
            return base::fail("chmod", std::format("cannot make {} executable: {}", path, failed.message()));
        }
    }
    return {};
}

} // namespace mcppls::platform::fs
