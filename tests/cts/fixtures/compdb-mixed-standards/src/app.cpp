import std;
import core;

int main() {
    auto [n, _] = std::pair { core_name().size(), 0 };
    std::println("{}", n + 1);
    return 0;
}
