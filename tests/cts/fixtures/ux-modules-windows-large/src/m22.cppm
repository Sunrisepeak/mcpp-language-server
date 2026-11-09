module;
#include <windows.h>
export module big.m22;
import big.m21;

export namespace m22 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m21::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window22 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m22
