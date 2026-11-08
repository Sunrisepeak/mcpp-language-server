module;
#include <windows.h>
export module big.m01;
import big.m00;

export namespace m01 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m00::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window01 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m01
