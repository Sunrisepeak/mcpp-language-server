module;
#include <windows.h>
export module big.m19;
import big.m18;

export namespace m19 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m18::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window19 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m19
