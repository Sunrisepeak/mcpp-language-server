#include "sys.h"
#include <openkal/version.h>

// What this implementation says about itself before it is used.
//
// NOT AN INTERFACE, AND SO NOT CONDITIONAL ON ONE. Every conforming
// implementation exports both names; clause 3.2's closure is of the set of core
// INTERFACES, and these provide no resource. A consumer that is linked learns an
// interface's absence from the linker, and one bound at load or across a
// boundary has no linker to ask --- so it asks here, before it calls anything.
//
// Both are constants on this implementation, which is what the specification
// says the cheap answer should be.
extern "C" {

kal_u64 kal_version(void) { return KAL_VERSION; }

kal_u64 kal_interfaces(void) {
    // Every interface this implementation provides. It is written out rather
    // than derived, because a word derived from what happens to be linked would
    // report a facility as present when the linker had merely kept it.
    return KAL_IFACE_ABORT  | KAL_IFACE_STREAM   | KAL_IFACE_MEMORY
         | KAL_IFACE_ENV    | KAL_IFACE_TIME     | KAL_IFACE_RANDOM
         | KAL_IFACE_FS     | KAL_IFACE_PROCESS  | KAL_IFACE_TASK
         | KAL_IFACE_EXEC   | KAL_IFACE_TERMINAL | KAL_IFACE_NET
         | KAL_IFACE_DATAGRAM | KAL_IFACE_SPACE  | KAL_IFACE_TIMEOUT;
}

}
