#include "report.hpp"

std::string describe(const std::vector<int>& values) {
    std::string text;
    for (const int value : values) text += std::to_string(value) + " ";
    return text;
}
