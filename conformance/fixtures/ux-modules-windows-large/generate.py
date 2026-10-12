#!/usr/bin/env python3
"""Writes the ux-modules-windows-large project (0.0.14 Windows plan, phase 0): thirty module units whose global module
fragments include <windows.h>, imported in chains four deep, and a main.cpp that includes <windows.h> and imports
twenty-five of them -- the shape of a real Windows program written with modules, generated so the repository holds
only this script. The lines main.cpp's checks name are fixed: see the scenario."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent
MODULES = 30
IMPORTED = 25

def unit(n):
    # m00, m05, m10, ... start a chain; the next four each import the one before.
    imports = f'import big.m{n - 1:02};\n' if n % 5 else ''
    previous = f' + m{n - 1:02}::value()' if n % 5 else ''
    return (f'module;\n#include <windows.h>\nexport module big.m{n:02};\n{imports}\n'
            f'export namespace m{n:02} {{\n'
            f'inline DWORD value() {{ return ::GetCurrentProcessId() % 7{previous}; }}\n'
            f'inline ULONGLONG ticks() {{ return ::GetTickCount64(); }}\n'
            f'struct Window{n:02} {{ HWND handle = nullptr; bool visible() const {{ return handle != nullptr; }} }};\n'
            f'}} // namespace m{n:02}\n')

def main_cpp():
    imports = ''.join(f'import big.m{n:02};\n' for n in range(MODULES - IMPORTED, MODULES))
    # Lines 0-1: includes; then the imports; the body's lines are what the checks point at.
    return ('#include <windows.h>\n\n' + imports + '\n'
            'int main() {\n'
            f'    DWORD total = m{MODULES - 1:02}::value();\n'
            f'    m{MODULES - 1:02}::Window{MODULES - 1:02} window;\n'
            '    if (window.visible()) total += 1;\n'
            '    ::Sleep(0);\n'
            '    return static_cast<int>(total);\n'
            '}\n')

def main():
    src = ROOT / 'src'
    src.mkdir(exist_ok=True)
    for n in range(MODULES):
        (src / f'm{n:02}.cppm').write_text(unit(n))
    (src / 'main.cpp').write_text(main_cpp())
    files = ' '.join(f'src/m{n:02}.cppm' for n in range(MODULES))
    (ROOT / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.28)\nproject(bigwin LANGUAGES CXX)\n\n'
        'set(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\n\n'
        f'add_library(big)\ntarget_sources(big PUBLIC FILE_SET CXX_MODULES FILES {files})\n'
        'target_compile_definitions(big PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)\n\n'
        'add_executable(app src/main.cpp)\ntarget_link_libraries(app PRIVATE big)\n')

if __name__ == '__main__':
    main()
