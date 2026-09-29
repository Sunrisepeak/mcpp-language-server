#include "sys.h"
#include <openkal/space.h>

// openkal.space upon clone(2).
//
// THE KERNEL'S PRIMITIVE IS THE INTERFACE'S OPERATION, WHICH IS WHY THE
// INTERFACE HAS ONE. clone with SIGCHLD and no CLONE_VM copies the address space
// and begins execution in the copy; there is no form that does the first without
// the second. An interface separating them would have obliged this file to park
// the started context and build a channel by which to tell it what to run, which
// clause 7.1 excludes. The specification was changed rather than this file.

extern "C" {

int kal_space_start(void (*entry)(void*), void* arg, void* stack_top,
                    kal_process* out) {
    if (entry == nullptr || out == nullptr) return kal_err_invalid;

    // THE STACK ARGUMENT IS NOT PASSED TO THE KERNEL, AND THE HEADER SAYS SO.
    //
    // Passing a stack to clone requires CLONE_VM --- the child would otherwise
    // be running on a copy of the caller's stack at an address the caller chose,
    // and the kernel's own copy of the stack is already correct. CLONE_VM is the
    // opposite of what this interface provides: it would share the address space
    // rather than copy it, which is openkal.task.
    //
    // So the copied stack is used, the argument is accepted and ignored, and the
    // header states that an implementation whose environment gives the started
    // context a stack of its own does exactly this. A caller cannot observe
    // which occurred and has no decision resting upon it.
    (void)stack_top;

    const okl_long child = okl::sys(okl::nr_clone, 17 /* SIGCHLD */, 0, 0, 0, 0);
    if (okl::failed(child)) return okl::translate(child);

    if (child == 0) {
        entry(arg);
        // THE ENTRY IS NOT REQUIRED TO RETURN, AND IF IT DOES THE CONTEXT ENDS.
        //
        // Returning from here would return into clone's caller in the copied
        // space, which is the whole address space of the program running a
        // second time from the middle of this function. Ending the context is
        // the only defined thing to do, and the status says the entry returned
        // rather than choosing one.
        okl::sys(okl::nr_exit_group, 0);
        for (;;) { }
    }

    *out = kal_process{ static_cast<kal_uintptr>(child) };
    return kal_ok;
}

// Both positions hold on this kernel.
//
// The handles accompany the memory: clone without CLONE_FILES gives the started
// context a copy of the descriptor table, so every handle the caller holds is
// open in the copy at the same number, and the packing in handle.h recovers the
// same descriptor from the same word.
//
// The copy is deferred: Linux maps the pages copy-on-write, so a store to copied
// memory can fail with the machine out of memory after this call has already
// reported success. An implementation cannot undefer that, and stating it is
// what lets a program that cannot tolerate it know which environment it is in.
kal_uintptr kal_space_props(void) {
    return KAL_SPACE_PROP_CLONE_HANDLES | KAL_SPACE_PROP_DEFERRED_COPY;
}

}  // extern "C"
