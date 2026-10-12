module;
#include <windows.h>
export module big.m15;

export namespace m15 {
inline DWORD value() { return ::GetCurrentProcessId() % 7; }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window15 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m15
