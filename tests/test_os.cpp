import std;
import mcppls.os;

int main() {
    using namespace mcppls::os;
    bool ok { true };
    if constexpr (FAMILY == Family::windows) {
        ok = EXECUTABLE_SUFFIX == ".exe" && PATH_LIST_SEPARATOR == ';' && VSCODE_TARGET == "win32-x64";
    } else if constexpr (FAMILY == Family::macos) {
        ok = EXECUTABLE_SUFFIX.empty() && PATH_LIST_SEPARATOR == ':' && VSCODE_TARGET == "darwin-arm64";
    } else {
        ok = EXECUTABLE_SUFFIX.empty() && PATH_LIST_SEPARATOR == ':' && VSCODE_TARGET == "linux-x64";
    }
    std::println("test_os: family={} ok={}", FAMILY_NAME, ok);
    return ok ? 0 : 1;
}
