module;
#include <windows.h>
export module big.m16;
import big.m15;

export namespace m16 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m15::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window16 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m16
