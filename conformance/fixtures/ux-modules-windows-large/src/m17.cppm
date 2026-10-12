module;
#include <windows.h>
export module big.m17;
import big.m16;

export namespace m17 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m16::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window17 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m17
