module mcppls.engine.clangd.bmi;

import std;

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
    std::string_view name { fileName };
    if (name.ends_with(".pcm")) name.remove_suffix(4);

    // The stamp is exactly three trailing fields: a serial, -HHMMSS, -YYYYMMDD. Each is checked for
    // shape, because a module name may itself contain '-' and splitting on the first one would cut
    // `my-module` in half. Anything that does not match all three is a name, not a stamp.
    const std::size_t lastDash { name.rfind('-') };
    if (lastDash == std::string_view::npos || lastDash + 1 >= name.size()) return std::string { name };
    for (std::size_t at { lastDash + 1 }; at < name.size(); ++at) {
        if (!std::isdigit(static_cast<unsigned char>(name[at]))) return std::string { name };
    }
    const std::size_t timeEnd { digit_field_before(name, lastDash, 6) };
    if (timeEnd == std::string_view::npos) return std::string { name };
    const std::size_t dateEnd { digit_field_before(name, timeEnd, 8) };
    if (dateEnd == std::string_view::npos) return std::string { name };
    return std::string { name.substr(0, dateEnd) };
}

} // namespace mcppls::engine::clangd
