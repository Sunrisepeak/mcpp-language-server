#include <windows.h>
#include <string>
import platform.process;
import platform.files;

int main() {
    platform::Process process;
    platform::Directory directory(L".");
    auto names = directory.entryNames();
    ::OutputDebugStringW(directory.path().c_str());
    SetLastError(0);
    return process.isCurrent() && !names.empty() ? 0 : 1;
}
