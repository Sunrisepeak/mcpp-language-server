export module calc:geometry;

import std;
import :types;

export namespace calc {
double dot(const vec2& a, const vec2& b) {
    return a.x * b.x + a.y * b.y;
}

double length(const vec2& v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}
}
