#pragma once
/* Independent, reproducible experiments; zero retains the previous build. */
#ifndef BENCH_OPTIMIZATIONS
#define BENCH_OPTIMIZATIONS 0
#endif
#define OPT_LEAN       (BENCH_OPTIMIZATIONS & 1)
#define OPT_SNS        (BENCH_OPTIMIZATIONS & 2)
#define OPT_SPEC       (BENCH_OPTIMIZATIONS & 4)
#define OPT_TRANSPORT  (BENCH_OPTIMIZATIONS & 8)
#define OPT_MDCT       (BENCH_OPTIMIZATIONS & 16)
#define OPT_RAM        (BENCH_OPTIMIZATIONS & 32)
#if OPT_RAM && defined(__ZEPHYR__)
#include <zephyr/toolchain.h>
#define CUSTOM_RAM __ramfunc
#else
#define CUSTOM_RAM
#endif
