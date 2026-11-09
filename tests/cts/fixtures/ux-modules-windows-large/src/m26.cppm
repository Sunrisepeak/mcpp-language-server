module;
#include <windows.h>
export module big.m26;
import big.m25;

export namespace m26 {
inline DWORD value() { return ::GetCurrentProcessId() % 7 + m25::value(); }
inline ULONGLONG ticks() { return ::GetTickCount64(); }
struct Window26 { HWND handle = nullptr; bool visible() const { return handle != nullptr; } };
} // namespace m26
