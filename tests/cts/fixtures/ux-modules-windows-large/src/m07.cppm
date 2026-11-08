module;
#include <windows.h>
export module big.m07;
import big.m06;

export namespace m07 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m06::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window07 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m07
