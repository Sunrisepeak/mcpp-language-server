module;
#include <windows.h>
export module big.m03;
import big.m02;

export namespace m03 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m02::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window03 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m03
