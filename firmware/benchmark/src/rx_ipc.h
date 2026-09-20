#pragma once

#include <stdint.h>
#include <stddef.h>

/* The packet passed from the ESB network-core receiver to the application
 * core.  A 2.5 ms custom-LC3 frame is below 204 bytes at the tested rates, so
 * one complete frame fits in a single RPMsg/IPC message. */
#define RX_IPC_MAGIC 0x52584950u
#define RX_IPC_MAX_FRAME 204u

enum rx_ipc_type {
    RX_IPC_START = 1,
    RX_IPC_FRAME = 2,
    RX_IPC_END = 3,
    RX_IPC_VOLUME = 4,
    RX_IPC_STATUS = 5,
};

struct rx_ipc_message {
    uint32_t magic;
    uint32_t run;
    uint32_t seq;
    uint32_t crc;
    uint16_t bytes;
    uint8_t type;
    uint8_t reserved;
    uint8_t data[RX_IPC_MAX_FRAME];
};

/* Kept separate from the frame message so the final report remains small. */
struct rx_app_status {
    uint32_t magic;
    uint32_t run;
    uint32_t expected;
    uint32_t decoded;
    uint32_t plc_frames;
    uint32_t decode_errors;
    uint32_t ipc_drops;
    uint32_t queue_peak;
    uint32_t decode_avg_us;
    uint32_t decode_max_us;
    uint32_t playback_late;
    uint32_t i2s_blocks;
    uint32_t i2s_errors;
    uint32_t i2s_underruns;
    int32_t i2s_last_error;
    uint32_t volume_updates;
    uint32_t volume_last;
    uint32_t accel_irqs;
    uint32_t accel_work;
    uint32_t i2c_attempts;
    uint32_t i2c_failures;
    uint32_t status_flags;
    uint32_t pcm_queue_min;
    uint32_t pcm_queue_max;
    uint32_t pcm_queue_last;
    uint32_t pcm_queue_early;
    uint32_t dma_released;
    uint32_t dma_span_us;
    uint32_t dma_gap_max_us;
    uint32_t arrival_span_us;
    uint32_t dac_irqs;
    uint32_t pcm_remaining;
    uint32_t clock_updates;
    int32_t clock_steps;
    int32_t clock_error_us;
    int32_t clock_peak_error_us;
    uint32_t clock_rejected;
    int32_t clock_test_bias;
};

/* Network-core side of the receiver IPC link. */
int rx_ipc_init(void);
int rx_ipc_send_start(uint32_t run, uint32_t frames);
int rx_ipc_send_frame(uint32_t run, uint32_t seq, const uint8_t *data,
                      unsigned bytes, uint32_t crc);
int rx_ipc_send_end(uint32_t run, uint32_t frames);
int rx_ipc_send_volume(uint32_t run, uint32_t seq, uint16_t level);
int rx_ipc_wait_status(struct rx_app_status *status, int timeout_ms);
