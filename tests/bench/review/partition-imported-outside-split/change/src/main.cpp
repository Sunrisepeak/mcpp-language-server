import std;
import hello.greet;
import hello.greet:format;

int main() {
    std::println("{}", hello::greet("mcpp"));
    return hello::count() > 0 ? 0 : 1;
}
