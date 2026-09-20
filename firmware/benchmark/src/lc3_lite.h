#pragma once
#include <stdint.h>
/* Experimental encoder flags. The decoder and wire syntax are unchanged. */
enum { LITE_NO_LTPF=1, LITE_NO_TNS=2, LITE_NO_GAIN_REFINE=4 };
enum { PROF_ATTACK, PROF_LTPF, PROF_MDCT, PROF_ENERGY, PROF_BW,
       PROF_SNS, PROF_TNS, PROF_SPEC, PROF_BITS, PROF_GAIN, PROF_COUNT };
extern unsigned lc3_lite_flags;
extern uint64_t lc3_lite_cycles[PROF_COUNT];
void lc3_lite_set_flags(unsigned flags);
#ifdef LC3_LITE_PROFILE
#include <cmsis_core.h>
#define LITE_MEASURE(slot, statement) do { \
    uint32_t lite_start_ = DWT->CYCCNT; \
    statement; \
    lc3_lite_cycles[slot] += (uint32_t)(DWT->CYCCNT - lite_start_); \
} while (0)
#else
#define LITE_MEASURE(slot, statement) do { statement; } while (0)
#endif
