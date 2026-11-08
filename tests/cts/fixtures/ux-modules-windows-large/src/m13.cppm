module;
#include <windows.h>
export module big.m13;
import big.m12;

export namespace m13 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m12::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window13 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m13
