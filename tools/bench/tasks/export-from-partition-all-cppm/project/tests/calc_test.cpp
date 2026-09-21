import std;
import calc;

int main() {
    if (calc::length(calc::vec2 { 3.0, 4.0 }) != 5.0) return 1;
    const calc::vec2 mid = calc::midpoint(calc::vec2 { 0.0, 0.0 }, calc::vec2 { 4.0, 2.0 });
    if (mid.x != 2.0 || mid.y != 1.0) return 1;
    return 0;
}
