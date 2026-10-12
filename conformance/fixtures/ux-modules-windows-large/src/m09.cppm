module;
#include <windows.h>
export module big.m09;
import big.m08;

export namespace m09 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m08::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window09 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m09
