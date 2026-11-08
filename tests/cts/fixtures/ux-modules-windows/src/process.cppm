module;
#include <windows.h>
export module platform.process;

export namespace platform {

struct Process {
    DWORD id = ::GetCurrentProcessId();
    ULONGLONG uptimeMilliseconds() const { return ::GetTickCount64(); }
    bool isCurrent() const { return id == ::GetCurrentProcessId(); }
};

inline unsigned long currentThreadId() { return ::GetCurrentThreadId(); }

} // namespace platform
