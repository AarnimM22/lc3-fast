#pragma once
#include "bench.h"
#define USB_PCM_BYTES 720
#define USB_WARMUP_FRAMES 100
struct usb_pcm_frame {
    uint64_t ready_us;
    uint32_t sequence;
    uint8_t data[USB_PCM_BYTES];
};
struct usb_pcm_run { uint32_t id, bitrate, frames; };
struct usb_pcm_stats {
    uint32_t packets, zero_packets, malformed, buffer_fail, overflows;
    uint32_t queued_frames, queue_peak, disconnects, callbacks, callback_max_cycles;
    uint64_t callback_cycles;
};
int usb_pcm_init(void);
int usb_pcm_wait_run(struct usb_pcm_run *run);
int usb_pcm_get(struct usb_pcm_frame **frame);
void usb_pcm_release(struct usb_pcm_frame *frame);
void usb_pcm_finish(struct usb_pcm_stats *stats);
int codec_encode_usb(unsigned sequence, const uint8_t *pcm, uint8_t *out, unsigned *bytes);
