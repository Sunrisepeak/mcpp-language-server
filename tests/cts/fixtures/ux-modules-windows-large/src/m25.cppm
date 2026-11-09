module;
#include <windows.h>
export module big.m25;

export namespace m25 {
inline DWORD value() { return ::GetCurrentProcessId() % 7; }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window25 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m25
