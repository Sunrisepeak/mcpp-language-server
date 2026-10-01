// Preprocessing this file reads wc15.h twice, so the chain from wc0.h reads its last file 2^21 times: the scan of a unit that includes
// wc0.h before a missing header takes that long to fail, and clangd's reports of the modules it cannot find come apart in time, as they do
// on a real project.
#include "wc15.h"
#include "wc15.h"
