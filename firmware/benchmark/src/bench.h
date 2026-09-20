#pragma once
#include <stdint.h>
#include <zephyr/kernel.h>
#include "bench_opts.h"
#define MAGIC 0x4c433350u
#define CHUNK 224
#define MAX_FRAME 4096
/* Both MCUs are little endian. Fixed-width header is 24 bytes. */
struct header {
    uint32_t magic, run, seq, crc;
    uint16_t bytes, offset;
    uint8_t type, reserved[3];
};
BUILD_ASSERT(sizeof(struct header) == 24);
enum { START = 1, DATA = 2, END = 3, VOLUME = 4 };
struct rx_packet { uint16_t length; uint8_t data[252]; };
extern struct k_msgq rx_queue;
extern uint32_t rx_overflow;
int radio_init(void);
int radio_send(const void *data, unsigned len);
#ifdef BENCH_HAS_ASYNC
/* One radio worker owns blocking ESB calls while the main thread encodes.
 * The bounded queue owns a copy of each payload until its ACK or failure. */
struct radio_async_stats {
    uint32_t completed, errors, late, peak_pending;
    uint32_t service_max_us, latency_max_us;
    uint64_t service_total_us, latency_total_us;
};
int radio_async_reset(void);
int radio_async_submit(const void *data, unsigned len, uint64_t ready_us,
                       uint64_t deadline_us);
int radio_async_drain(struct radio_async_stats *stats);
#if OPT_TRANSPORT
/* Ownership transfers to commit(), including its error paths. The worker
 * releases the packet only after radio_send() has finished using it. */
struct radio_owned_packet {
    uint64_t ready_us, deadline_us;
    uint16_t length;
    uint8_t data[252];
};
struct radio_owned_packet *radio_async_acquire(void);
void radio_async_cancel(struct radio_owned_packet *packet);
int radio_async_commit(struct radio_owned_packet *packet);
uint32_t bench_crc32(const uint8_t *data, unsigned length);
#endif
#endif
extern uint32_t tx_ok, tx_fail, tx_retries;
int codec_init(unsigned frame_us, unsigned bitrate);
int codec_encode(unsigned sequence, uint8_t *out, unsigned *bytes);
const char *codec_name(void);
extern uint32_t codec_api_cycles;
#ifdef CODEC_WAVPACK
extern unsigned wavpack_fast;
#endif
#ifdef CODEC_OPUS
extern unsigned opus_pcm_bits, opus_complexity;
#endif
static inline uint64_t now_us(void) { return k_ticks_to_us_floor64(k_uptime_ticks()); }
