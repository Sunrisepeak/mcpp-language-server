module;
#include <Windows.h>
export module win_demo;

export inline DWORD tick() { return GetTickCount(); }
