#include "shapes.hpp"

int area(const Rect& rect) { return rect.width * rect.height; }

int perimeter(const Rect& rect) { return 2 * (rect.width + rect.height); }
