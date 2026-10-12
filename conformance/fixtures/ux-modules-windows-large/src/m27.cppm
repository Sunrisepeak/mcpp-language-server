module;
#include <windows.h>
export module big.m27;
import big.m26;

export namespace m27 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m26::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window27 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m27
