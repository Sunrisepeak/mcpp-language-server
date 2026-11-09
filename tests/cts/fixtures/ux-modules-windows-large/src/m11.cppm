module;
#include <windows.h>
export module big.m11;
import big.m10;

export namespace m11 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m10::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window11 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m11
