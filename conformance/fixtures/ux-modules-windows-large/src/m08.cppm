module;
#include <windows.h>
export module big.m08;
import big.m07;

export namespace m08 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m07::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window08 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m08
