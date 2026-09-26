import std;
import sat;

void old_api() = delete("use clamp_add");   // C++26: = delete("reason") (P2573)

int main() {
    auto [a, _] = std::pair { 1, 2 };        // C++26: placeholder variables (P2169)
    auto [b, _] = std::pair { 3, 4 };
    first_of<int, double> x = clamp_add(a, b);
    std::println("{}", x);
    return 0;
}
