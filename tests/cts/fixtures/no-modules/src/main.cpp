#include <iostream>

#include "report.hpp"
#include "shapes.hpp"

int main() {
    const Rect room { 3, 4 };
    std::cout << area(room) << " " << perimeter(room) << "\n";
    std::cout << describe({ area(room), perimeter(room) }) << "\n";
}
