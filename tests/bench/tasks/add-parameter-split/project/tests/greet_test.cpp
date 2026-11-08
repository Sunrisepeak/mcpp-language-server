import std;
import hello.greet;

int main() {
    return hello::greet("test", "Hi") == "[Hi, test]" ? 0 : 1;
}
