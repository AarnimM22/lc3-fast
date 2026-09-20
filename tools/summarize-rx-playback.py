"""Validate a completed USB -> ESB -> IPC -> decoder -> I2S bench run.

DMA releases imply completed audio blocks only when the I2S error counters
are zero. This is a finite-duration DK timing test, not an acoustic test.
"""
import argparse
import json
import re
from pathlib import Path


def record(path, tag, run, id_key="run"):
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if not line.startswith(tag + " "):
            continue
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        if fields.get(id_key) == str(run):
            return {k: int(v) if re.fullmatch(r"-?\d+", v) else v
                    for k, v in fields.items()}
    raise ValueError(f"Missing {tag} for run {run} in {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--run", type=int, required=True)
    parser.add_argument("--require-servo", action="store_true",
                        help="Require a >=60 s clock-recovery run with bounded lead error")
    parser.add_argument("--require-stress", action="store_true",
                        help="Also require the deliberate negative PLL bias to have been applied")
    args = parser.parse_args()
    rxlog, txlog = (args.directory / "rx-capture.log",
                    args.directory / "tx-capture.log")
    rx = record(rxlog, "RX_RESULT", args.run)
    clock = record(rxlog, "RX_CLOCK", args.run)
    playback = record(rxlog, "RX_PLAYBACK", args.run)
    tx = record(txlog, "USB_RESULT", args.run, "id")
    usb = record(txlog, "USB_INPUT", args.run, "id")
    n = rx["expected"]
    checks = {
        "all_radio_frames": rx["good"] == n and tx["frames"] == n,
        "all_decoded": rx["app_decoded"] == n,
        "all_i2s_queued": rx["app_i2s_blocks"] == n,
        "all_dma_released": clock["dma_released"] == n,
        "drained": clock["pcm_remaining"] == 0,
        "all_volume_updates": rx["app_volume_updates"] == (n + 49) // 100,
        "sensor_load_present": rx["app_accel_irqs"] > n * 2.4,
        "sensor_work_present": rx["app_accel_work"] > n * 2.0,
        "dac_irq_load_present": clock["dac_irqs"] > n * 0.24,
        "i2c_load_present": rx["app_i2c_attempts"] >= n // 16,
        "pcm_buffer_bounded": 2 <= playback["pcm_slab_min"] <=
            playback["pcm_slab_max"] <= 8,
    }
    servo = None
    if args.require_servo or args.require_stress:
        servo = record(rxlog, "RX_SERVO", args.run)
        checks["servo_duration"] = n >= 24000
        checks["servo_updates"] = servo["updates"] >= n // 400 - 10
        checks["servo_not_saturated"] = abs(servo["steps"]) < 60
        checks["servo_final_lead_within_1ms"] = abs(servo["error_us"]) < 1000
        checks["servo_peak_lead_within_frame"] = servo["peak_error_us"] < 2500
        checks["servo_observations_valid"] = servo["rejected"] == 0
        if args.require_stress:
            checks["servo_stress_injected"] = servo.get("bias") == -30
    for key in ("missing", "bad", "overflow", "ipc_send_errors", "app_wait",
                "app_plc", "app_decode_errors", "app_ipc_drops",
                "app_playback_late", "app_i2s_errors", "app_i2s_underruns",
                "app_i2s_last_error"):
        checks["rx_zero_" + key] = rx[key] == 0
    for key in ("sequence_errors", "codec_errors", "send_errors", "timeout",
                "timing_errors", "ack_fail"):
        checks["tx_zero_" + key] = tx[key] == 0
    for key in ("malformed", "buffer_fail", "overflows", "disconnects"):
        checks["usb_zero_" + key] = usb[key] == 0
    span = clock["dma_span_us"]
    arrival_span = clock["arrival_span_us"]
    result = {
        "passed": all(checks.values()), "checks": checks,
        "rx": rx, "tx": tx, "usb": usb, "clock": clock,
        "playback": playback,
        "servo": servo,
        "dma_mean_block_us": span / (n - 1) if n > 1 else None,
        "arrival_minus_dma_span_us": arrival_span - span,
        "endpoint_span_difference_ppm":
            (arrival_span - span) / span * 1e6 if span else None,
        "note": "Endpoint span difference includes arrival jitter and RTC quantization; "
                "it is not a calibrated oscillator measurement. Slab occupancy includes "
                "queued, current and next DMA blocks and is sampled after PCM writes. "
                "I2C failures are expected with no peripheral connected.",
    }
    path = args.directory / "validation.json"
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ("rx", "tx", "usb", "clock", "playback")}, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
