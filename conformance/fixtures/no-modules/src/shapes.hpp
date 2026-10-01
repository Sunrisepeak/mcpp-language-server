#pragma once

struct Rect {
    int width;
    int height;
};

// The area of a rectangle in square units.
int area(const Rect& rect);
int perimeter(const Rect& rect);
