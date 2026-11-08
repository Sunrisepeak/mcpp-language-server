module;
#include <windows.h>
export module big.m14;
import big.m13;

export namespace m14 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m13::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window14 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m14
