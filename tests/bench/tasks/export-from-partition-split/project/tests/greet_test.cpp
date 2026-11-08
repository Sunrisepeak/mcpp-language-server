import std;
import hello.greet;

int main() {
    if (hello::greet("test").empty()) return 1;
    if (hello::shout("hi") != "HI!!!") return 1;
    return 0;
}
