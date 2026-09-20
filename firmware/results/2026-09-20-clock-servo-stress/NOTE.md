This directory's name reflects the intended test, not the firmware actually
run. Run 9601 had AUDIO_CLOCK_STRESS=0 because the sysbuild option was not
imported by the application-core CMake file. It is a valid 250-second heavy
audio test of normal clock recovery, and passes `--require-servo`, but it
does not test injected clock disturbances. APP_CLOCK and RX_SERVO confirm
stress=0 and bias=0. The option propagation was fixed before run 9602 in
the sibling `2026-09-20-clock-servo-disturbance` directory.
