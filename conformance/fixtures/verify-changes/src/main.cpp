import std;
import hello.greet;

int main() {
    std::println("{}", hello::greet("mcpp"));
    return hello::count() > 0 ? 0 : 1;
}
