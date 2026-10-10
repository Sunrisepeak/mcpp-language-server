module;
#include <windows.h>
#include <string>
#include <vector>
export module platform.files;

export namespace platform {

class Directory {
public:
    explicit Directory(std::wstring path) : path_(std::move(path)) {}
    std::vector<std::wstring> entryNames() const {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW found {};
        HANDLE handle = ::FindFirstFileW((path_ + L"\\*").c_str(), &found);
        if (handle == INVALID_HANDLE_VALUE) return names;
        do names.emplace_back(found.cFileName); while (::FindNextFileW(handle, &found));
        ::FindClose(handle);
        return names;
    }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

} // namespace platform
