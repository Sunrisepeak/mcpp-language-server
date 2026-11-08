module;
#include <windows.h>
export module big.m20;

export namespace m20 {
inline DWORD value() { return ::GetCurrentProcessId() % 7; }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window20 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m20
