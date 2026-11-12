// Projects described by a compile database: CMake's for each compiler (clang, clang-cl, MSVC, a
// build database), others' (mixed standards, a command clangd refuses, a database being written),
// and MinGW's through a sysroot.
import mcppls.cts;

using namespace mcpplibs::mtest;
using namespace mcppls::cts;

namespace {

Fixture cmake_clang{"cmake-clang", on{"linux-x64"}};
Fixture cmake_clang_bdb{"cmake-clang-bdb", on{"linux-x64"}};
Fixture cmake_clang_cl{"cmake-clang-cl", on{"win32-x64"}};
Fixture cmake_clangxx_msvc{"cmake-clangxx-msvc", on{"win32-x64"}};
Fixture cmake_fetchcontent_offline{"cmake-fetchcontent-offline", on{"linux-x64"}};
Fixture cmake_msvc{"cmake-msvc", on{"win32-x64"}};
Fixture cmake_msvc_bdb{"cmake-msvc-bdb", on{"win32-x64"}};
// Its project opens CMake 3.31.6's experimental `import std` (the gate's UUID, and how the standard module is
// linked): a row whose image has another CMake leaves it out by this tag (ci.yml, the build-26100 row).
Fixture cmake_msvc_std{"cmake-msvc-std", on{"win32-x64"}, tag{"cmake-3.31-import-std"}};
Fixture compdb_clang_cl_std{"compdb-clang-cl-std", on{"win32-x64"}};
Fixture compdb_clangxx_msvc_std{"compdb-clangxx-msvc-std", on{"win32-x64"}};
Fixture compdb_lto_msvc{"compdb-lto-msvc", on{"linux-x64"}};
Fixture compdb_midwrite{"compdb-midwrite", on{"linux-x64"}};
Fixture compdb_mixed_standards{"compdb-mixed-standards", on{"linux-x64"}};
Fixture compdb_rejected_command{"compdb-rejected-command", on{"linux-x64"}};
Fixture mingw{"mingw", on{"linux-x64", "win32-x64"}};

}  // namespace
