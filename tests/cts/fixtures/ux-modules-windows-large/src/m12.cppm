module;
#include <windows.h>
export module big.m12;
import big.m11;

export namespace m12 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m11::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window12 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m12
