import std;
import hello.greet;

int main() {
    return hello::salute("test").empty() ? 1 : 0;
}
