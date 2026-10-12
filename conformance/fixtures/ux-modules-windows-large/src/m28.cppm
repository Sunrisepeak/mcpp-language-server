module;
#include <windows.h>
export module big.m28;
import big.m27;

export namespace m28 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m27::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window28 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m28
