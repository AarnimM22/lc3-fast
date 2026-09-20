# 384 kb/s USB-to-I2S playback workload validation

Date: 2026-09-20. nRF52840 DK 000683088082 transmits USB-sourced custom LC3-fast; nRF5340 DK 001050038165 receives on the network core and decodes on the application core at 64 MHz. Both finite runs passed all checks in `validation.json`. Processing, transport, and I2S coexist under the tested concurrent load. Independent source/playback clocks still require rate correction for indefinite operation.

## Results

| Measurement | Full source, run 9490 | Demanding excerpt, run 9500 |
|---|---:|---:|
| Audio duration | 250 s | 50 s |
| Frames received / decoded / DMA released | 100,000 / 100,000 / 100,000 | 20,000 / 20,000 / 20,000 |
| Configured codec rate | 384 kb/s | 384 kb/s |
| Actual mean compressed audio payload rate | 270.596 kb/s | 381.413 kb/s |
| RX decode plus volume time, mean / maximum | 1.333 / 1.740 ms | 1.550 / 1.739 ms |
| TX work time, mean / maximum | 1.815 / 2.808 ms | 2.186 / 2.717 ms |
| TX work exceeding 2.5 ms | 1,047 frames | 151 frames |
| Radio retries / terminal ACK failures | 15 / 0 | 3 / 0 |
| Ready-to-ACK maximum | 5.890 ms | 5.371 ms |
| Missing frames / decoder errors / I2S underruns | 0 / 0 / 0 | 0 / 0 / 0 |
| Wireless volume commands applied | 1,000 | 200 |
| Simulated sensor interrupts / work executions | 248,245 / 240,150 | 49,652 / 48,019 |
| Simulated DAC interrupts | 24,976 | 4,995 |
| I2C attempts / failures (no attached target) | 7,250 / 7,250 | 1,450 / 1,450 |
| PCM slab ownership, minimum / maximum / final | 3 / 7 / 7 | 3 / 5 / 4 |
| Maximum DMA release gap | 2.594 ms | 2.564 ms |

All USB overflow, malformed-packet, buffer-failure and disconnect counters were zero. All IPC drops, receive overflow, codec errors, send errors and stream timeouts were zero. Every queued PCM block was released and the stream drained with zero blocks remaining. Occasional TX work longer than one frame period was absorbed by the existing buffering; this is not evidence that every individual frame fits a 2.5 ms execution deadline.

The codec uses variable payload lengths. The long source contains easier passages, so its configured rate is not its average air payload rate. The additional excerpt run sustains approximately the requested payload rate. Rates above exclude ESB framing, acknowledgements, retries, volume packets and other overhead.

## Clock drift and buffer interpretation

In run 9490, the first-to-last arrival span was 250,000,458 us, versus 250,008,117 us for DMA releases: arrivals gained 7,659 us, approximately 30.6 ppm. Early slab ownership was four blocks and final ownership was seven. This is consistent with source audio arriving faster than playback consumes it. At that measured difference, approximately one 2.5 ms frame accumulates per 82 seconds. Run 9500 showed a 1,252 us span difference, approximately 25 ppm. Endpoint jitter and RTC timestamp quantization affect these estimates; these are not calibrated oscillator measurements.

Startup prefill is three decoded frames (7.5 ms of PCM). The allocation pool has eight blocks and the I2S queue has eight entries. Ownership includes software-queued, current and next DMA blocks, sampled after writes; it is not pure unplayed-frame occupancy or measured end-to-end latency. This test therefore does not establish a strict three-frame total receiver-buffer requirement.

No clock servo, ASRC, sample insertion or sample deletion was enabled. A fixed buffer will eventually fill under persistent positive drift. Implementing receiver clock correction or resampling is required before claiming indefinite stable playback; increasing a fixed buffer only delays the problem.

## Workload and implementation

Actual I2S hardware and EasyDMA run continuously during playback at nominal 48 kHz stereo, with 24 significant PCM bits left-aligned in 32-bit slots. ACLK is 12.288 MHz, MCK bypass is enabled, and MCK/LRCK is 256. Native 24-bit slots cannot use this bypass combination; the final 32-bit-slot configuration preserves all 24 significant bits. Playback starts after three actual decoded blocks. Assertions are enabled.

Volume commands travel over the same wireless link at four per second and change software PCM gain. The app also submits I2C requests. A nominal 1 ms timer submits sensor work containing 48 integer iterations; queued work can coalesce. A nominal 10 ms timer spends 20 us in interrupt context to model DAC interrupt load. Real I2C controller writes target address 0x30 every 16 audio frames and on volume changes, with a 2 ms timeout.

The application-core workload did not prevent the network-core ESB path from acknowledging packets in these runs. Ready-to-ACK time includes queuing and transmission and must not be interpreted as the radio's ACK turnaround time. No deliberately delayed-ACK or controlled RF-interference experiment was performed.

There is no external CS43131 or accelerometer attached. I2C failures are expected and do not validate successful device transactions. Timer callbacks model workload, not actual device GPIO interrupt timing or priorities. No pin waveform capture, analog output, acoustic-quality test or controlled Wi-Fi/Bluetooth coexistence test was performed. The maximum observed decoder execution leaves about 0.76 ms within a 2.5 ms frame period, but that is observed execution headroom, not a universal CPU-utilization guarantee.

## Evidence and reproduction

This directory contains `rx-capture.log`, `tx-capture.log`, `validation.json`, application-core RTT, saved app/network build configurations, build log and `firmware-sha256.json`. The second run is in sibling directory `../2026-09-20-rx-appcore-384-heavy-20k/`, with its own captures and validation. Both runs used the same firmware.

The main source is `firmware/benchmark/24_48k_PerfectTest.wav`, looped by the host. The demanding source `.bench/playback-heavy-5s.wav` contains its first 240,000 stereo sample frames, copied as packed 24-bit PCM at 48 kHz without resampling. Recreate that excerpt from the original before reproducing the second command.

From the workspace root, with the tested firmware flashed:

```powershell
.\tools\run-usb-rate-sweep.ps1 -Rates 384000 -Frames 100000 -RunBase 9490 -OutputDirectory firmware\results\new-long-run
.\tools\run-usb-rate-sweep.ps1 -Rates 384000 -Frames 20000 -RunBase 9500 -OutputDirectory firmware\results\new-heavy-run -WavPath .bench\playback-heavy-5s.wav
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\summarize-rx-playback.py firmware\results\new-long-run --run 9490
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\summarize-rx-playback.py firmware\results\new-heavy-run --run 9500
```

See `firmware/benchmark/app_core/README.md` for build/flash instructions, pin assignments and instrumentation semantics. Use new run IDs and output directories for new measurements.
