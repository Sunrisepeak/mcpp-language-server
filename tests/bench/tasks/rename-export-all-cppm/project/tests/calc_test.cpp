import std;
import calc;

int main() {
    return calc::magnitude(calc::vec2 { 3.0, 4.0 }) == 5.0 ? 0 : 1;
}
