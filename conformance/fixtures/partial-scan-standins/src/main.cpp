import std;
import standin.a;
import standin.b;
import standin.c;

int main(int argc, char* argv[]) {
    std::println("{}", standin::value_a() + standin::value_b() + standin::value_c());
    return 0;
}
