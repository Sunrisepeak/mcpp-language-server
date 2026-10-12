module;
#include <windows.h>
export module big.m02;
import big.m01;

export namespace m02 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m01::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window02 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m02
