// Preprocessing this file reads wb19.h twice, so the chain from wb0.h reads its last file 2^20 times: the scan of a unit that includes
// wb0.h before a missing header takes that long to fail, and clangd's reports of the modules it cannot find come apart in time, as they do
// on a real project.
#include "wb19.h"
#include "wb19.h"
