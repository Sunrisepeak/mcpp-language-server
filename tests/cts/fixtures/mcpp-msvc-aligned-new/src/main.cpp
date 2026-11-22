#include "vt.h"
import std;
import app.shapes;

int main() {
    int* value = vt::make<int>();
    std::println("{} {}", app::describe(), *value);
    vt::destroy(value);
    return 0;
}
