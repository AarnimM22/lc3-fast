/* The reference excludes this portable helper on ARM but supplies no ARM
 * replacement. Identical arithmetic to basop_mpy.c's non-ARM definition. */
#include "functions.h"
Word32 Mpy_32_16_0_0(Word32 x, Word16 y)
{
    return L_shr_pos(Mpy_32_16(x, y), 1);
}
