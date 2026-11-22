module;
#include "vt.h"
export module app.shapes;
import std;

export namespace app {
struct alignas(32) Shape { int sides { 3 }; };
std::string describe() {
    Shape* shape = vt::make<Shape>();
    std::string text { std::format("{} sides", shape->sides) };
    vt::destroy(shape);
    return text;
}
} // namespace app
