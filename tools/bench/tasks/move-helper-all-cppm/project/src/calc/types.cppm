export module calc:types;

export namespace calc {
struct vec2 {
    double x;
    double y;
};

double norm_squared(const vec2& v) {
    return square(v.x) + square(v.y);
}
}
