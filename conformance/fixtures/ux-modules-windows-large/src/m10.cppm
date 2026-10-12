module;
#include <windows.h>
export module big.m10;

export namespace m10 {
inline DWORD value() { return ::GetCurrentProcessId() % 7; }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window10 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m10
