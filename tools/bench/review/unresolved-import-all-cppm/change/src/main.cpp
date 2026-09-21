import std;
import calc;
import calc.missing;

int main() {
    const calc::vec2 v { 3.0, 4.0 };
    std::println("{} {}", calc::length(v), calc::format_pair(v.x, v.y));
    return 0;
}
