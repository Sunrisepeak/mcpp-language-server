module hello.greet;

import std;

namespace hello {
std::string decorate(std::string_view text) {
    return std::format("[{}]", text);
}
}
