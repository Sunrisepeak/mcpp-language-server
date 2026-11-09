module;
#include <windows.h>
export module big.m21;
import big.m20;

export namespace m21 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m20::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window21 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m21
