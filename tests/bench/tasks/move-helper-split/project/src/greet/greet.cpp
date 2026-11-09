module hello.greet;

import std;
import :counter;

namespace {
std::string normalize(std::string_view text) {
    std::string result(text);
    if (!result.empty() && result[0] >= 'a' && result[0] <= 'z') {
        result[0] = static_cast<char>(result[0] - 'a' + 'A');
    }
    return result;
}
}

namespace hello {
std::string greet(std::string_view who) {
    detail::bump();
    return decorate(std::format("hello, {}", normalize(who)));
}

int count() {
    return detail::value();
}
}
