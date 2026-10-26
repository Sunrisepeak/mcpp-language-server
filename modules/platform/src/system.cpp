module mcppls.platform.system;

import std;
import mcpplibs.os;

namespace mcppls::platform::system {

std::optional<std::uint64_t> physical_memory_bytes() { return mcpplibs::os::machine::memory(); }

// A question of Windows' volumes (mcpplibs.os.windows), defined only where Windows is the target; an
// ok image, which may run there too, does not ask it.
std::optional<bool> on_dev_drive(std::string_view directory) {
    if constexpr (mcpplibs::os::is_windows) return mcpplibs::os::windows::on_dev_drive(std::filesystem::path { std::string { directory } });
    else return std::nullopt;
}

} // namespace mcppls::platform::system
