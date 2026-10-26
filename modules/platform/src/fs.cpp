module mcppls.platform.fs;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcpplibs.os;
import mcppls.platform.dirs;
import mcppls.os;

namespace mcppls::platform::fs {

namespace {

std::filesystem::path native(std::string_view path) { return std::filesystem::path { std::string { path } }; }

std::string from_native(const std::filesystem::path& path) { return base::normalize_path(path.generic_string()); }


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

base::Result<std::string> read_file(std::string_view path, std::size_t maxBytes) {
    std::ifstream stream { native(path), std::ios::binary };
    if (!stream) return base::fail("read-file", std::format("cannot open {}", path));
    std::string content;
    std::array<char, 4096> buffer;
    while (true) {
        const auto remaining = maxBytes - content.size();
        const auto amount = remaining >= buffer.size() ? buffer.size() : remaining + 1;
        stream.read(buffer.data(), static_cast<std::streamsize>(amount));
        content.append(buffer.data(), static_cast<std::size_t>(stream.gcount()));
        if (content.size() > maxBytes) return base::fail("read-file-limit", std::format("{} exceeds the {} byte read limit", path, maxBytes));
        if (stream.bad() || (stream.fail() && !stream.eof())) return base::fail("read-file", std::format("cannot read {}", path));
        if (stream.eof()) return content;
    }
}

base::Result<void> write_file(std::string_view path, std::string_view content) {
    std::ofstream stream { native(path), std::ios::binary | std::ios::trunc };
    if (!stream) return base::fail("write-file", std::format("cannot create {}", path));
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
    if (!stream) return base::fail("write-file", std::format("cannot write {}", path));
    return {};
}

// mcpplibs.os's: a sibling with a name no other writer takes, made durable, then renamed over the
// name in one step, which replaces an existing file on every system.
base::Result<void> write_file_atomic(std::string_view path, std::string_view content) {
    if (auto written = mcpplibs::os::fs::write_atomic(native(path), content); !written)
        return base::fail("write-file", std::format("cannot replace {}: {}", path, written.error().message()));
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

base::Result<void> remove(std::string_view path) {
    std::error_code error;
    std::filesystem::remove(native(path), error);
    if (error) return base::fail("remove", std::format("cannot remove {}: {}", path, error.message()));
    return {};
}

Removal tree_remove(std::string_view path) {
    Removal removed;
    if (!is_directory(path)) {
        if (const auto stamp = fs::stamp(path)) removed.bytes = stamp->size;
        if (auto taken = remove(path); !taken) ++removed.failed;
        return removed;
    }
    for (const auto& entry : list_directory(path)) {
        const Removal nested { tree_remove(entry) };
        removed.bytes += nested.bytes;
        removed.failed += nested.failed;
    }
    if (auto taken = remove(path); !taken) ++removed.failed;   // the now-empty directory itself
    return removed;
}

std::int64_t modified_now() {
    // No clock_cast on this standard library, and none needed: a file just written carries the
    // file clock's own reading of now. The scratch file lives in the temporary directory, one
    // name reused; its stamp is what "now" means to `stamp`.
    static const std::string scratch { base::join_path(
        [] {
            const std::string temporary { platform::dirs::temp_directory() };
            (void)create_directories(temporary);
            return temporary;
        }(),
        ".mcppls-clock") };
    (void)write_file(scratch, "now");
    const auto stamp = fs::stamp(scratch);
    return stamp ? stamp->modified : std::int64_t { 0 };
}

base::Result<void> rename(std::string_view from, std::string_view to) {
    std::error_code error;
    std::filesystem::rename(native(from), native(to), error);
    if (error) return base::fail("rename", std::format("cannot move {} to {}: {}", from, to, error.message()));
    return {};
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
    const auto identity = mcpplibs::os::fs::identity_of(native(path));
    if (!identity) return std::nullopt;
    return FileIdentity { identity->volume, identity->index };
}

std::string canonical_path(std::string_view path) {
    if (path.empty()) return {};
    if (base::native_path_style() == base::PathStyle::windows) {
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

// Whether a node may be started is recorded by some volumes and not by others (openkal
// KAL_FS_PROP_EXECUTABLE): Windows' decide by the extension, so there is nothing to grant and
// nothing that could have been withheld, and that answer is success. Asked of mcpplibs.os rather
// than through std::filesystem::permissions, which on such a volume reported an error and failed
// every extraction that held one executable (the fault address was "chmod\n": the category of the
// error read as a pointer).
base::Result<void> make_executable(std::span<const std::string> paths) {
    for (const auto& path : paths) {
        auto marked = mcpplibs::os::fs::set_executable(native(path), true);
        if (!marked && marked.error().code != mcpplibs::os::errc::not_supported)
            return base::fail("chmod", std::format("cannot make {} executable: {}", path, marked.error().message()));
    }
    return {};
}

} // namespace mcppls::platform::fs
