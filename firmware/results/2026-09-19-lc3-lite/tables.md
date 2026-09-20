# LC3-lite sweep

| Variant | kb/s | Local API avg/max, us | Local wrapper P99, us | Concurrent RF work avg/max, us | Late/1000 |
|---|---:|---:|---:|---:|---:|
| Baseline | 256 | 3316/3737 | 3773 | 3722/4120 | 1000 |
| Baseline | 320 | 3377/3815 | 3834 | 3797/4212 | 1000 |
| No LTPF | 256 | 2458/2856 | 2889 | 2866/3357 | 1000 |
| No LTPF | 320 | 2517/2936 | 2998 | 2937/3418 | 1000 |
| No TNS | 256 | 3145/3458 | 3595 | 3550/3937 | 1000 |
| No TNS | 320 | 3206/3522 | 3650 | 3626/3998 | 1000 |
| No LTPF/TNS | 256 | 2286/2509 | 2642 | 2693/2991 | 1000 |
| No LTPF/TNS | 320 | 2345/2569 | 2698 | 2767/3052 | 1000 |
| No gain refinement | 256 | 3272/3695 | 3717 | 3676/4059 | 1000 |
| No gain refinement | 320 | 3335/3737 | 3777 | 3754/4120 | 1000 |
| No LTPF/gain refinement | 256 | 2414/2792 | 2812 | 2820/3265 | 1000 |
| No LTPF/gain refinement | 320 | 2475/2840 | 2882 | 2896/3327 | 1000 |
| All three disabled | 256 | 2243/2455 | 2591 | 2650/2930 | 1000 |
| All three disabled | 320 | 2304/2500 | 2647 | 2725/2991 | 1000 |

## Synthetic waveform SNR (diagnostic only)

Delay aligned; not a perceptual score, listening result or effective-bit-depth estimate.

| Signal | kb/s | Baseline | No LTPF | No TNS | No gain refinement | All three disabled |
|---|---:|---:|---:|---:|---:|---:|
| tones | 256 | 18.97 | 61.31 | 18.97 | 18.98 | 60.13 |
| tones | 320 | 64.88 | 65.27 | 63.70 | 63.64 | 62.54 |
| multitone | 256 | 16.42 | 22.77 | 16.28 | 12.52 | 14.34 |
| multitone | 320 | 24.70 | 22.37 | 25.27 | 17.10 | 14.73 |
| chirp | 256 | 43.79 | 43.65 | 44.34 | 30.20 | 33.70 |
| chirp | 320 | 60.30 | 60.38 | 46.96 | 33.13 | 33.05 |
| quiet_24bit | 256 | 44.84 | 44.84 | 44.83 | 43.99 | 43.99 |
| quiet_24bit | 320 | 48.52 | 48.52 | 48.41 | 48.20 | 48.07 |
| noise | 256 | 5.32 | 5.33 | 5.32 | 5.06 | 5.08 |
| noise | 320 | 6.77 | 6.78 | 6.77 | 6.43 | 6.43 |
| percussion | 256 | 5.26 | 5.30 | 5.22 | 5.07 | 5.07 |
| percussion | 320 | 6.82 | 6.84 | 6.76 | 6.52 | 6.44 |
| clicks | 256 | 6.10 | 6.10 | 5.71 | 5.63 | 5.73 |
| clicks | 320 | 7.28 | 7.28 | 6.71 | 7.25 | 6.60 |
| harmonics | 256 | 18.99 | 44.94 | 18.98 | 18.99 | 37.87 |
| harmonics | 320 | 51.03 | 51.64 | 46.70 | 50.29 | 40.50 |
