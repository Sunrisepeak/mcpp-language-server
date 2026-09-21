module hello.greet;

import std;
import :counter;

namespace hello {
std::string greet(std::string_view who) {
    detail::bump();
    return decorate(std::format("hi, {}", who));
}

int count() {
    return detail::value();
}
}
