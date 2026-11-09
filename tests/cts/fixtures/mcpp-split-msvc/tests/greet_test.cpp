import std;
import hello.greet;

int main() {
    return hello::greet("test").empty() ? 1 : 0;
}
