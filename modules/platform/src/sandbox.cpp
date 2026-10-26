module mcppls.platform.sandbox;

import std;
import mcpplibs.os;

namespace mcppls::platform {

const std::string& sandbox() {
    static const std::string answer { [] {
        if constexpr (mcpplibs::os::is_linux) return mcpplibs::os::linux::translator().value_or(std::string {});
        else return std::string {};
    }() };
    return answer;
}

} // namespace mcppls::platform
