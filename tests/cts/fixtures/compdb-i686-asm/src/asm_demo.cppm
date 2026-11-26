export module asm_demo;

// MS-style inline assembly, as winnt.h's Int64ShllMod32 has it for a 32-bit Windows target.
export inline unsigned long long shift_left(unsigned long long value, unsigned count) {
    __asm {
        mov ecx, count
        shl dword ptr value, cl
    }
    return value;
}
