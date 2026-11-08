export module calc:geometry;

import std;
import :types;

export namespace calc {
double length(const vec2& v, double scale) {
    return scale * std::sqrt(v.x * v.x + v.y * v.y);
}
}
