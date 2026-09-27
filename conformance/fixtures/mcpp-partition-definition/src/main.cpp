import std;
import hello.greet;

int main() {
    hello::Counter c;
    c.bump();
    hello::Point p{1, 2};
    std::println("{} {} {} {} {}", hello::greet("mcpp"), hello::add(1, 2), c.value(), p.sum(), hello::shout("x"));
    std::println("{} {}", hello::twice(3), hello::run_all());
    std::println("{}", hello::configure(true));
}
