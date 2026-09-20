#include "lc3_lite.h"
#include <string.h>
unsigned lc3_lite_flags;
uint64_t lc3_lite_cycles[PROF_COUNT];
void lc3_lite_set_flags(unsigned flags)
{
    lc3_lite_flags = flags;
    memset(lc3_lite_cycles, 0, sizeof(lc3_lite_cycles));
}
