import std;
import calc;

int main() {
    const calc::vec2 v { 3.0, 4.0 };
    std::println("{} {}", calc::length(v, 1.0), calc::format_pair(v.x, v.y));
    return 0;
}
