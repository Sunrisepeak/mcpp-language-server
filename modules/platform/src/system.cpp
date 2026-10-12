module mcppls.platform.system;

import std;
import mcppls.base.text;
import mcppls.platform.fs;

// The Windows calls are declared the way process.cpp declares its own: exactly what is used, `dllimport`
// and `__stdcall`, since this package reaches the system through openkal and includes no vendor header.
#if defined(__MCPP_TARGET_WINDOWS__)
extern "C" {
using KalHandle = void*;
using KalDword = unsigned long;
using KalBool = int;
struct KalMemoryStatusEx {
    KalDword length;
    KalDword memoryLoad;
    unsigned long long totalPhysical;
    unsigned long long availablePhysical;
    unsigned long long totalPageFile;
    unsigned long long availablePageFile;
    unsigned long long totalVirtual;
    unsigned long long availableVirtual;
    unsigned long long availableExtendedVirtual;
};
// FILE_FS_PERSISTENT_VOLUME_INFORMATION
struct KalPersistentVolumeInformation {
    KalDword volumeFlags;
    KalDword flagMask;
    KalDword version;
    KalDword reserved;
};
__declspec(dllimport) KalBool __stdcall GlobalMemoryStatusEx(KalMemoryStatusEx*);
__declspec(dllimport) KalHandle __stdcall CreateFileW(const wchar_t*, KalDword, KalDword, void*, KalDword, KalDword, KalHandle);
__declspec(dllimport) KalBool __stdcall DeviceIoControl(KalHandle, KalDword, void*, KalDword, void*, KalDword, KalDword*, void*);
__declspec(dllimport) KalBool __stdcall CloseHandle(KalHandle);
}
namespace {
constexpr KalDword KAL_FILE_READ_ATTRIBUTES { 0x0080 };
constexpr KalDword KAL_FILE_SHARE_ALL { 0x1 | 0x2 | 0x4 };
constexpr KalDword KAL_OPEN_EXISTING { 3 };
constexpr KalDword KAL_FILE_FLAG_BACKUP_SEMANTICS { 0x02000000 };
constexpr KalDword KAL_FSCTL_QUERY_PERSISTENT_VOLUME_STATE { 0x0009023C };
constexpr KalDword KAL_PERSISTENT_VOLUME_STATE_DEV_VOLUME { 0x00002000 };
const KalHandle KAL_INVALID_HANDLE { reinterpret_cast<KalHandle>(static_cast<std::intptr_t>(-1)) };

// UTF-8 to UTF-16, enough for a path; nullopt on a malformed sequence.
std::optional<std::wstring> widen(std::string_view text) {
    std::wstring out;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::uint32_t code { 0 };
        std::size_t length { 1 };
        if (lead < 0x80) code = lead;
        else if ((lead & 0xE0) == 0xC0) { code = lead & 0x1F; length = 2; }
        else if ((lead & 0xF0) == 0xE0) { code = lead & 0x0F; length = 3; }
        else if ((lead & 0xF8) == 0xF0) { code = lead & 0x07; length = 4; }
        else return std::nullopt;
        if (i + length > text.size()) return std::nullopt;
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) return std::nullopt;
            code = (code << 6) | (next & 0x3F);
        }
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(code));
        }
        i += length;
    }
    return out;
}
} // namespace
#endif

namespace mcppls::platform::system {

std::optional<std::uint64_t> physical_memory_bytes() {
#if defined(__MCPP_TARGET_WINDOWS__)
    KalMemoryStatusEx status {};
    status.length = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return std::nullopt;
    return static_cast<std::uint64_t>(status.totalPhysical);
#else
    const auto info = fs::read_file("/proc/meminfo");
    if (!info) return std::nullopt;
    for (const auto line : base::split_lines(*info)) {
        if (!line.starts_with("MemTotal:")) continue;
        std::uint64_t kib { 0 };
        const auto digits = base::trim(line.substr(9));
        const auto number = digits.substr(0, digits.find(' '));
        if (std::from_chars(number.data(), number.data() + number.size(), kib).ec != std::errc {}) return std::nullopt;
        return kib * 1024;
    }
    return std::nullopt;
#endif
}

std::optional<bool> on_dev_drive(std::string_view directory) {
#if defined(__MCPP_TARGET_WINDOWS__)
    auto wide = widen(directory);
    if (!wide) return std::nullopt;
    // The FSCTL goes to the file system of whatever is opened: the directory itself will do.
    const KalHandle handle { CreateFileW(wide->c_str(), KAL_FILE_READ_ATTRIBUTES, KAL_FILE_SHARE_ALL, nullptr, KAL_OPEN_EXISTING,
                                         KAL_FILE_FLAG_BACKUP_SEMANTICS, nullptr) };
    if (handle == KAL_INVALID_HANDLE || handle == nullptr) return std::nullopt;
    KalPersistentVolumeInformation info {};
    info.version = 1;
    info.flagMask = KAL_PERSISTENT_VOLUME_STATE_DEV_VOLUME;
    KalDword returned { 0 };
    const bool asked { DeviceIoControl(handle, KAL_FSCTL_QUERY_PERSISTENT_VOLUME_STATE, &info, sizeof(info), &info, sizeof(info), &returned,
                                       nullptr) != 0 };
    CloseHandle(handle);
    if (!asked) return std::nullopt;
    return (info.volumeFlags & KAL_PERSISTENT_VOLUME_STATE_DEV_VOLUME) != 0;
#else
    (void)directory;
    return std::nullopt;
#endif
}

} // namespace mcppls::platform::system
