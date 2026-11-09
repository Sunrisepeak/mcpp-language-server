module;
#include <windows.h>
export module big.m23;
import big.m22;

export namespace m23 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m22::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window23 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m23
