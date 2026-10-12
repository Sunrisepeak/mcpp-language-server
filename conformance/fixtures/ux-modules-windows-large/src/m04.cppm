module;
#include <windows.h>
export module big.m04;
import big.m03;

export namespace m04 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m03::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window04 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m04
