module;
#include <stddef.h>
export module format.demo;
import std;
export namespace demo {
template <class T>
requires requires(T x) { x + x; }
T sum(T first, T second) {
    auto combine = [&](T left, T right) { return left + right; };
    return combine(first, second);
}
class Buffer {
public:
    Buffer(int capacity, int limit) : capacity_(capacity), limit_(limit) {}
    int scaled() const {
        return capacity_ * limit_ + capacity_ * limit_ + capacity_ * limit_ + capacity_ * limit_ +
               capacity_ * limit_;
    }
private:
    int capacity_;
    int limit_;
};
}  // namespace demo
