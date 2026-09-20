# nRF5340 receiver playback workload

The nRF5340 network core receives ESB audio and acknowledges it through the
Nordic ESB driver. It forwards complete custom-LC3 packets and volume commands
over RPMsg to this application. The application core decodes 48 kHz stereo
S24 PCM, applies volume, and queues 120-sample stereo blocks to I2S EasyDMA.

## I2S configuration

- 48 kHz; 32-bit stereo I2S slots containing 24 significant bits and eight
  zero padding bits. No input precision is discarded.
- ACLK 12.288 MHz, MCK bypass enabled, MCK/LRCK ratio 256.
- MCK P0.12, SCK P1.15, LRCK P1.12, SDOUT P1.13. SDIN P1.14 is configured
  in pinctrl but no receive stream is started.
- Start after three decoded PCM blocks have been queued, rather than starting
  on the wireless START marker with silence that can expire before audio.
- Eight PCM slab blocks and eight driver queue entries. Three is the startup
  prefill, not a hard limit on all DMA-owned blocks or the end-to-end latency.
- The link-time slab-free wrapper observes DMA buffer releases in interrupt
  context. SDK files are not patched. Release counts are interpreted as played
  blocks only when I2S errors are zero and the stream drains fully.

Native 24-bit slot mode cannot use the exact 256x bypass ratio: nrfx rejects
that combination. Disabling bypass is not sufficient to establish exact
48 kHz. The tested configuration instead uses supported 32-bit slots.

## Concurrent work

- Wireless volume packet every 100 audio frames (4 updates/s).
- A nominal 1 ms Zephyr timer submits sensor-filter work (48 integer iterations).
  Queued work may coalesce; both interrupt and work counts are reported.
- A nominal 10 ms timer models DAC status interrupts with a 20 us busy wait
  in interrupt context. These are synthetic timer callbacks, not physical
  accelerometer/DAC GPIO interrupts with their device-specific priorities.
- I2C1 attempts a four-byte write to address 0x30 every 16 audio frames and on
  volume changes. The DK has no attached DAC; these attempts fail or time out.
  The controller timeout is bounded to 2 ms so the system workqueue remains
  responsive. Successful device transactions and the CS43131 register protocol
  require a later test with the real board.

## Run

From the workspace root, using the existing nRF52840 USB benchmark transmitter:

```powershell
& .\tools\audio-bench.ps1 -Action build -Target rx -Codec etsi -Frames 200
& .\tools\audio-bench.ps1 -Action flash -Target rx -Codec etsi
& .\tools\run-usb-rate-sweep.ps1 -Rates 384000 -Frames 100000 -RunBase 9490 -OutputDirectory firmware\results\new-playback-run
& 'C:\ncs\toolchains\66cdf9b75e\opt\bin\python.exe' tools\summarize-rx-playback.py firmware\results\new-playback-run --run 9490
```

Choose a new output directory and run ID for each capture. The legacy `etsi`
receiver build name does not select an ETSI decoder: this child image builds
the matching custom-LC3 decoder from generated liblc3 sources.

## Interpretation

`RX_CLOCK` contains DMA-release count, first-to-last release span, maximum
release gap, first-to-last IPC frame arrival span, and remaining PCM blocks
after draining. `RX_PLAYBACK` contains slab ownership sampled after each PCM
write; ownership includes queued/current/next blocks, so it is not a count
of complete unplayed frames. `RX_RESULT` includes decode times, errors and load
counters. TX USB statistics are captured separately.

Arrival-span minus DMA-span indicates finite-run clock drift plus arrival
jitter and RTC timestamp quantization. The audio clock servo described below
now regulates playback rate. There is no resampling or sample insertion/deletion.
A successful finite run does not prove indefinite operation in every environment.
The software deadline counter is a secondary diagnostic; DMA completions,
I2S errors and buffer occupancy are the playback evidence.

This test drives actual I2S peripheral/DMA activity on the DK. It does not
capture the pin waveform or test acoustic output, actual DAC interrupts,
CS43131 I2C acknowledgements, or controlled RF interference.

## Audio clock recovery

The USB host supplies the source timing. The nRF52840 forwards sample-numbered
frames without added synchronization packets or processing. On the nRF5340,
the application estimates source sample position minus I2S DMA position at
each incoming frame. Fractional progress since the last DMA release is included
to avoid quantizing the observation to a complete 2.5 ms block. Observations
use the local RTC; they are not calibrated radio hardware timestamps.

After ignoring the first second, five one-second means establish the startup
lead. Subsequent one-second means drive a PI controller (P=1/10 s,
I=1/400 s^2) with a 50 us deadband. This deadband reflects the 30.5 us RTC
resolution and averaged transport jitter; it is not a 4 us radio-phase lock.
The integral retains the learned frequency inside the deadband and is bounded
to 150 ppm. Output is bounded to approximately 200 ppm and slewed by at most
four PLL register steps per update (approximately 13.25 ppm/s).

The actuator directly changes HFCLKAUDIO.FREQUENCY within the 12.288 MHz
band while I2S runs, as supported by Nordic's nRF5340 audio oscillator. Each
register step changes rate by approximately 3.311 ppm. No audio samples are
altered, repeated or discarded by this controller. I2S remains the clock master
on its physical pins, while its average rate follows the USB source.

Hardware reference: [Nordic nRF5340 audio oscillator](https://docs.nordicsemi.com/r/bundle/ps_nrf5340/page/chapters/clock/doc/clock.html-concepthfclkaudio).
The local NCS 3.2.1 `nrf5340_audio/src/audio/audio_datapath.c` also adjusts
HFCLKAUDIO at runtime for drift compensation. This implementation owns the
frequency register while streaming; another clock controller must not also
write it. It does not retune either MCU's HFXO or its radio clock.

Sequence discontinuities, gaps of 100 ms or greater, and stale DMA observations
discard the partial measurement window. The last frequency correction is held;
START resets the controller and PLL to nominal. The existing benchmark's PLC
and stream-ending logic are unchanged. Transport-latency changes can bias the
lead estimate, so sustained RF outages, reacquisition and more complex source
changes still require separate validation.

`RX_SERVO` reports update count, final register offset, final/peak averaged
lead error and rejected observations. `APP_SERVO` provides ten-second samples
in application-core RTT. Slab occupancy and DMA completion counters remain the
independent playback checks.

For an explicit disturbance test, build RX with `-ClockStress`. It adds a
+30-register-step bias after 44 controller updates and changes that to -30
after 119 updates (roughly 50 and 125 seconds into playback). These offsets
are about +99.3 and -99.3 ppm; the second transition is about -198.7 ppm.
The estimator does not receive the bias as feed-forward information. `bias`
in RX/APP_SERVO exposes this test setting. A normal build without the switch
sets `AUDIO_CLOCK_STRESS=0` again. Always restore normal firmware after testing.
