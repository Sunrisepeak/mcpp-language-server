import std;
import calc;

int main() {
    const calc::vec2 v { .y = 4.0, .x = 3.0 };
    std::println("{} {}", calc::length(v), calc::format_pair(v.x, v.y));
    return 0;
}
