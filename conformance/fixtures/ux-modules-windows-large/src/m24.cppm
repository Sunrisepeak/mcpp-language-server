module;
#include <windows.h>
export module big.m24;
import big.m23;

export namespace m24 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m23::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window24 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m24
