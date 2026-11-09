import std;
import calc;

int main() {
    return calc::length(calc::vec2 { 3.0, 4.0 }, 2.0) == 10.0 ? 0 : 1;
}
