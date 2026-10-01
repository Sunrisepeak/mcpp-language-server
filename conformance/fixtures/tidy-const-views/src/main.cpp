#include <ranges>
#include <vector>

int count_even(const std::vector<int>& values) {
    auto evens = values | std::views::filter([](int v) { return v % 2 == 0; });
    auto doubled = evens | std::views::transform([](int v) { return v * 2; });
    int limit = 100;
    int total = 0;
    for (const int v : doubled) {
        if (total + v <= limit) total += v;
    }
    return total;
}
