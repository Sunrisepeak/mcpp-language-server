module;
#include <windows.h>
export module big.m00;

export namespace m00 {
inline DWORD value() { return ::GetCurrentProcessId() % 7; }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window00 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m00
