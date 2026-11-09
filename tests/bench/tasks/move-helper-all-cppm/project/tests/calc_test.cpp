import std;
import calc;

int main() {
    if (calc::length(calc::vec2 { 3.0, 4.0 }) != 5.0) return 1;
    if (calc::norm_squared(calc::vec2 { 3.0, 4.0 }) != 25.0) return 1;
    return 0;
}
