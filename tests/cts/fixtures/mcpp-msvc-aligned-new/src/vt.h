// The shape of Slint 1.19's slint/private/vtable.h (issue #97): aligned allocation used unconditionally, as C++17 allows.
#pragma once
#include <cstddef>
#include <new>

namespace vt {
template <typename T> T* make() {
    void* memory = ::operator new(sizeof(T), static_cast<std::align_val_t>(alignof(T)));
    return ::new (memory) T {};
}
template <typename T> void destroy(T* object) {
    object->~T();
    ::operator delete(static_cast<void*>(object), sizeof(T), static_cast<std::align_val_t>(alignof(T)));
}
} // namespace vt
