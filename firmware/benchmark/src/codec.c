#include "bench.h"
#include <string.h>
#include <math.h>
#include <cmsis_core.h>
#ifdef CODEC_LC3_LITE
#include "lc3_lite.h"
#endif
#ifdef CODEC_LC3_CUSTOM
#include "lc3_custom.h"
BUILD_ASSERT(BENCH_PCM_BITS == 24);
static unsigned custom_bitrate;
#endif
#if CODEC_ETSI
#include <lc3plus.h>
static uint8_t state[LC3PLUS_ENC_MAX_SIZE] __aligned(8);
uint8_t bench_codec_scratch[LC3PLUS_ENC_MAX_SCRATCH_SIZE + 4096] __aligned(8);
#define scratch_mem bench_codec_scratch
static LC3PLUS_Enc *enc = (LC3PLUS_Enc *)state;
static lc3_scratch_t scratch;
#else
#include <lc3.h>
static lc3_encoder_mem_48k_t state[2];
static lc3_encoder_t enc[2];
static unsigned channel_bytes;
#endif
static unsigned samples;
static int16_t pcm[2][480], tone[2][480];
static int32_t pcm24[2][480];
static uint32_t rng = 0x48125340;
uint32_t codec_api_cycles;
#ifdef BENCH_USB
/* Let the existing codec loader consume packed interleaved PCM directly.
 * Same sample values and rate allocation as the synthetic benchmark. */
int codec_encode_usb(unsigned sequence, const uint8_t *input,
                     uint8_t *out, unsigned *bytes)
{
    uint32_t start=DWT->CYCCNT;
    unsigned offset=0;
    for (unsigned ch=0;ch<2;ch++) {
        uint64_t block=(uint64_t)sequence+ch;
        int target=(block+1)*custom_bitrate/6400-block*custom_bitrate/6400;
        int rc=lc3_custom_encode(enc[ch],LC3_PCM_FORMAT_S24_3LE,input+3*ch,2,
                                target,out+offset,CUSTOM_MAX_CHANNEL);
        if(rc<0) { codec_api_cycles=DWT->CYCCNT-start; return rc; }
        offset+=rc;
    }
    codec_api_cycles=DWT->CYCCNT-start;
    *bytes=offset;
    return 0;
}
#endif
BUILD_ASSERT(BENCH_PCM_BITS == 16 || BENCH_PCM_BITS == 24);
const char *codec_name(void) {
#if CODEC_ETSI
    return "ETSI-1.9.2-fixed-TS1.7.1";
#elif defined(CODEC_LC3_CUSTOM)
    return "Google-8e1e722-CUSTOM-SNS-VBR-v1";
#elif defined(CODEC_LC3_LITE)
    return "Google-8e1e722-LC3plus-experimental-lite";
#else
    return "Google-8e1e722-LC3plus";
#endif
}
int codec_init(unsigned frame_us, unsigned bitrate)
{
    samples = frame_us * 48 / 1000;
#ifdef CODEC_LC3_CUSTOM
    custom_bitrate=bitrate;
#endif
    rng = 0x48125340;
    for (unsigned ch=0; ch<2; ch++) for (unsigned i=0; i<480; i++)
        tone[ch][i] = (int16_t)(12000.f * sinf(6.283185307f * (ch ? 17 : 10) * i / 480.f));
#if CODEC_ETSI
    int32_t scratch_size = 0;
    int rc = lc3plus_enc_init(enc, 48000, 2, NULL, &scratch_size);
    if (!rc) rc = lc3plus_enc_set_frame_dms(enc, (LC3PLUS_FrameDuration)(frame_us / 1250));
    if (!rc) rc = lc3plus_enc_set_bitrate(enc, bitrate);
    int required = scratch_size;
    printk("MEM state=%d scratch=%d allocated=%u init=%d\n", lc3plus_enc_get_size(48000, 2), required, (unsigned)sizeof(scratch_mem), rc);
    if (required > sizeof(scratch_mem)) return -ENOMEM;
    scratch = (lc3_scratch_t)scratch_mem;
    return rc;
#else
    if (frame_us < 2500) return -ENOTSUP;
    channel_bytes = bitrate * (uint64_t)frame_us / 16000000;
    for (unsigned ch=0; ch<2; ch++) {
        enc[ch] = lc3_setup_encoder(frame_us, 48000, 48000, &state[ch]);
        if (!enc[ch]) return -EINVAL;
#ifdef CODEC_LC3_LITE
        if (lc3_lite_flags & LITE_NO_LTPF) lc3_encoder_disable_ltpf(enc[ch]);
#endif
    }
    printk("MEM state=%u\n", 2*lc3_encoder_size(frame_us, 48000));
    return 0;
#endif
}
int codec_encode(unsigned sequence, uint8_t *out, unsigned *bytes)
{
    /* Deterministic 4-part content: tones, noise, silence, transient mixture.
     * DWT timing includes this small PCM generation overhead. */
    for (unsigned ch=0; ch<2; ch++) for (unsigned i=0; i<samples; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        unsigned phase = (sequence / 25) % 4;
        int16_t x = tone[ch][(sequence * samples + i) % 480];
        x = phase == 0 ? x : phase == 1 ? (int16_t)rng / 2 : phase == 2 ? 0 : ((sequence+i)%7 == 0 ? (int16_t)rng : x);
        pcm[ch][i] = x;
        pcm24[ch][i] = (int32_t)x * 256 + (phase == 2 ? 0 : (rng & 255));
    }
#if CODEC_ETSI
    int16_t *p[2] = {pcm[0], pcm[1]};
    int32_t *p24[2] = {pcm24[0], pcm24[1]};
    int n = 0;
    uint32_t start = DWT->CYCCNT;
    int rc = BENCH_PCM_BITS == 24 ? lc3plus_enc24(enc, p24, out, &n, scratch)
                                : lc3plus_enc16(enc, p, out, &n, scratch);
    codec_api_cycles = DWT->CYCCNT - start;
    *bytes = n;
    return rc;
#else
    uint32_t start = DWT->CYCCNT;
#ifdef CODEC_LC3_CUSTOM
    unsigned offset=0;
#endif
    for (unsigned ch=0; ch<2; ch++) {
#ifdef CODEC_LC3_CUSTOM
        /* Alternate fractional byte allocation without losing the 400 kb/s
         * target to integer division; channel headers count against bitrate. */
        uint64_t block=(uint64_t)sequence+ch;
        int target=(block+1)*custom_bitrate/6400-block*custom_bitrate/6400;
        int rc=lc3_custom_encode(enc[ch],LC3_PCM_FORMAT_S24,pcm24[ch],1,
                                target,out+offset,CUSTOM_MAX_CHANNEL);
        if (rc<0) { codec_api_cycles=DWT->CYCCNT-start; return rc; }
        offset+=rc;
#else
        int rc = lc3_encode(enc[ch], BENCH_PCM_BITS == 24 ? LC3_PCM_FORMAT_S24 : LC3_PCM_FORMAT_S16,
                            BENCH_PCM_BITS == 24 ? (const void *)pcm24[ch] : (const void *)pcm[ch],
                            1, channel_bytes, out+ch*channel_bytes);
        if (rc) { codec_api_cycles = DWT->CYCCNT - start; return rc; }
#endif
    }
    codec_api_cycles = DWT->CYCCNT - start;
#ifdef CODEC_LC3_CUSTOM
    *bytes=offset;
#else
    *bytes = channel_bytes * 2;
#endif
    return 0;
#endif
}
