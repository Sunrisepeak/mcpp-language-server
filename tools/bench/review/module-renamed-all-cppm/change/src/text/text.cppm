export module calc.format;

import std;

export namespace calc {
std::string format_pair(double x, double y) {
    return std::format("({}, {})", x, y);
}
}
