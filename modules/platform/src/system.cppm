// What the machine is: its memory, and what the volume a directory lives on is. Asked once at startup and
// cheap where the platform answers directly; nullopt where it does not.
export module mcppls.platform.system;

import std;

export namespace mcppls::platform::system {

// The machine's physical memory in bytes: /proc/meminfo on Linux, GlobalMemoryStatusEx on Windows.
std::optional<std::uint64_t> physical_memory_bytes();

// Windows: whether `directory` is on a Dev Drive (a ReFS volume Microsoft Defender scans asynchronously, in its
// performance mode). nullopt elsewhere, or when the volume cannot be asked.
std::optional<bool> on_dev_drive(std::string_view directory);

} // namespace mcppls::platform::system
