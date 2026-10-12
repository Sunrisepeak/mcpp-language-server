module;
#include <windows.h>
export module big.m18;
import big.m17;

export namespace m18 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m17::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window18 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m18
