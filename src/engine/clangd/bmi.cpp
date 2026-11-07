module mcppls.engine.clangd.bmi;

import std;
import mcpplibs.mwrap.xxhash;
import mcppls.base.path;
import mcppls.base.text;
import mcpplibs.os;
namespace mcppls::engine::clangd {

namespace {

// The `-`-separated field of `name` ending at `end`: its start index when it is exactly `count`
// digits preceded by a '-', npos otherwise.
std::size_t digit_field_before(std::string_view name, std::size_t end, std::size_t count) {
    if (end < count + 1 || name[end - count - 1] != '-') return std::string_view::npos;
    for (std::size_t at { end - count }; at < end; ++at) {
        if (!std::isdigit(static_cast<unsigned char>(name[at]))) return std::string_view::npos;
    }
    return end - count - 1;
}

} // namespace

std::string module_of_bmi(std::string_view fileName) {
    std::string_view whole { fileName };
    if (whole.ends_with(".pcm")) whole.remove_suffix(4);
    std::string_view name { whole };
    // A copy under a read lease names its lease `<stamped name>-XXXXXX.pcm.lease` (a unique file made
    // from the stamped name), and the copy is the lease's name without `.lease`: `<stamped name>.-XXXXXX`.
    // The suffix is a lease's only where a stamp precedes it; otherwise it is part of the name.
    if (const std::size_t lease { name.rfind(".-") }; lease != std::string_view::npos && lease + 2 < name.size()
        && std::ranges::all_of(name.substr(lease + 2), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }))
        name = name.substr(0, lease);
    const auto unstamped = [&] { return std::string { whole }; };

    // The stamp is exactly three trailing fields: a serial, -HHMMSS, -YYYYMMDD. Each is checked for
    // shape, because a module name may itself contain '-' and splitting on the first one would cut
    // `my-module` in half. Anything that does not match all three is a name, not a stamp.
    const std::size_t lastDash { name.rfind('-') };
    if (lastDash == std::string_view::npos || lastDash + 1 >= name.size()) return unstamped();
    for (std::size_t at { lastDash + 1 }; at < name.size(); ++at) {
        if (!std::isdigit(static_cast<unsigned char>(name[at]))) return unstamped();
    }
    const std::size_t timeEnd { digit_field_before(name, lastDash, 6) };
    if (timeEnd == std::string_view::npos) return unstamped();
    const std::size_t dateEnd { digit_field_before(name, timeEnd, 8) };
    if (dateEnd == std::string_view::npos) return unstamped();
    return std::string { name.substr(0, dateEnd) };
}

bool is_versioned_copy(std::string_view fileName, std::string_view directory) {
    std::error_code ignored;
    std::string_view name { fileName };
    if (name.ends_with(".pcm")) name.remove_suffix(4);
    const std::string module { module_of_bmi(fileName) };
    if (module == name) return false;   // no stamp: a name, not a copy
    // The stamp alone is shape, not proof; the canonical BMI beside it is -- the file the stamp was
    // taken FROM, so the lookup asks for the module's own name, never for the stamped file itself.
    return std::filesystem::exists(base::join_path(directory, std::format("{}.pcm", module)), ignored);
}

std::string unit_directory_of_bmi(std::string_view path) {
    std::string directory { base::parent_path(path) };
    if (base::file_name(directory).starts_with("producer-chain-v2-")) directory = base::parent_path(directory);
    return base::parent_path(directory);
}

std::string unit_lock_name(std::string_view sourcePath) {
    namespace os = mcpplibs::os;
    std::string path { base::normalize_path(sourcePath) };
    // clangd folds a path where the file system ignores case (Windows, macOS), and writes Windows's in its own style.
    if (os::host() == os::platform::windows) std::ranges::replace(path, '/', '\\');
    if (os::host() != os::platform::linux) path = base::to_lower_ascii(path);
    std::uint64_t digest { XXH3_64bits(path.data(), path.size()) };
    std::string name;
    for (int byte = 0; byte < 8; ++byte, digest >>= 8) name += std::format("{:02X}", static_cast<unsigned>(digest & 0xff));
    return name;
}

} // namespace mcppls::engine::clangd
