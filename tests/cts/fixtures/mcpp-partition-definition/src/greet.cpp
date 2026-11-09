module hello.greet;
import std;

namespace hello {
std::string greet(std::string_view name) {
    return std::format("hello, {}", name);
}
void Counter::bump() { ++n_; }
int Counter::value() const { return n_; }
}
