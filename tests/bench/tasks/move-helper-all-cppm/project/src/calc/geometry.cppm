export module calc:geometry;

import std;
import :types;

namespace {
double square(double x) {
    return x * x;
}
}

export namespace calc {
double length(const vec2& v) {
    return std::sqrt(square(v.x) + square(v.y));
}
}
