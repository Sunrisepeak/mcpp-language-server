module;
#include <windows.h>
export module big.m06;
import big.m05;

export namespace m06 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m05::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window06 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m06
