module hello.greet;

import std;
import :counter;

namespace hello {
std::string greet(std::string_view who) {
    detail::bump();
    return decorate(42);
}

int count() {
    return detail::value();
}
}
