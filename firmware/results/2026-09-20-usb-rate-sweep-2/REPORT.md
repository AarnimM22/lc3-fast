# nRF52840 intermediate bitrate sweep

The same nRF52840 USB Audio transmitter image was used for all rows:

- nRF52840DK serial `683088082`
- current custom lc3-fast encoder, optimization mask 31
- 48 kHz, stereo, packed 24-bit USB PCM
- 2.5 ms frames
- same USB input queue, radio pool and receiver as the earlier 320/400 tests
- 20,000 frames per rate (50 seconds of audio)

The host was `24_48k_PerfectTest.wav`, and the nRF5340 receiver acknowledged every
packet. There were zero USB overflows, codec errors, send errors, ACK failures,
sequence errors or receiver packet losses at every tested rate.

| Target | API avg/max (ms) | Whole work avg/max (ms) | Work > 2.5 ms | Producer late | USB queue peak | Radio pending peak | Ready-to-ACK max (ms) | Radio late | Retries |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 320 | 1.559 / 2.320 | 1.732 / 2.594 | 23 / 60,000 | 907 | 1 | 2 | 5.340 | 1 | 58 |
| 336 | 1.619 / 2.352 | 1.768 / 2.625 | 25 / 20,000 | 322 | 1 | 2 | 5.554 | 3 | 27 |
| 352 | 1.669 / 2.566 | 1.793 / 2.686 | 96 / 20,000 | 413 | 1 | 1 | 4.669 | 0 | 1 |
| 368 | 1.694 / 2.579 | 1.811 / 2.717 | 136 / 20,000 | 554 | 1 | 2 | 4.669 | 0 | 20 |
| 384 | 1.709 / 2.617 | 1.826 / 2.747 | 214 / 20,000 | 1,261 | 1 | 1 | 5.249 | 34 | 1 |
| 392 | 1.715 / 2.623 | 1.833 / 2.777 | 242 / 20,000 | 2,863 | 1 | 2 | 5.554 | 76 | 15 |
| 396 | 1.718 / 2.638 | 1.836 / 2.777 | 246 / 20,000 | 4,034 | 1 | 2 | 5.707 | 129 | 18 |
| 400 | 1.722 / 2.649 | 1.840 / 2.778 | 246 / 20,000 | 5,038 | 2 | 2 | 7.141 | 197 | 223 |

`Radio late` means an ACK completed after the five-millisecond diagnostic deadline;
it is not a lost packet. The receiver still reported 20,000 good packets in every row.
`USB queue peak` is the number of PCM frames waiting in the firmware input queue;
`radio pending peak` is the number of packets occupying the radio worker/pool path.

## 396 kb/s long soak

Because 396 kb/s was the highest intermediate rate that still had a one-frame USB
queue in the short sweep, it was repeated for 60,000 frames (150 seconds):

| Metric | 396 kb/s, 60,000 frames |
|---|---:|
| Encoder API average / maximum | 1.701 / 2.641 ms |
| Whole work average / maximum | 1.818 / 3.051 ms |
| Work intervals above 2.5 ms | 701 |
| Producer-late frames | 11,602 |
| USB queue peak | 1 |
| Radio pending peak | 3 |
| Ready-to-ACK average / maximum | 2.830 / 7.355 ms |
| Radio-late completions | 416 |
| ESB retries | 403 |
| USB overflows / codec errors / send errors | 0 / 0 / 0 |
| Receiver good / missing / bad / overflow | 60,000 / 0 / 0 / 0 |

Three 2.5 ms frames provide 7.5 ms. The 396 kb/s run stayed below that at 7.355 ms;
the earlier 400 kb/s 150-second run reached 8.118 ms and had a USB queue peak of 2.
The 396 result therefore establishes the highest tested rate that fit the same observed
three-frame latency envelope on this run. It has only 145 us of maximum-latency margin,
so 384 kb/s is the more conservative deployment setting if RF conditions vary.

This is a throughput and queueing result on the supplied PCM file, not a worst-case RF
guarantee. The custom VBR encoder's actual payload is below its target ceiling; packet
headers, retries and radio contention add further airtime. The 396 kb/s firmware image
still uses the original fixed pools. No buffer was enlarged for this sweep.

Logs and host metadata are in this directory. The extended 396 soak is in
`../2026-09-20-usb-rate-sweep-396-soak/`. The runner is
`tools/run-usb-rate-sweep.ps1`; it now accepts intermediate rates through the same USB
control command. The only firmware source change for this experiment is widening the
test harness's accepted bitrate range from exactly 320/400 to 320..400 kb/s in
`firmware/benchmark/src/usb_pcm.c`.
