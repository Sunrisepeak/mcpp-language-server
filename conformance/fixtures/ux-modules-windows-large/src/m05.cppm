module;
#include <windows.h>
export module big.m05;

export namespace m05 {
inline DWORD value() { return ::GetCurrentProcessId() % 7; }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window05 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m05
