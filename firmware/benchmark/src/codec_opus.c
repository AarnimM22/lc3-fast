#include "bench.h"
#include <opus.h>
#include <cmsis_core.h>
#include <math.h>

static uint8_t state[98304] __aligned(8);
static OpusEncoder *enc = (OpusEncoder *)state;
static int16_t pcm16[960], tone[2][480];
static int32_t pcm24[960];
static unsigned samples, packet_bytes;
static uint32_t rng;
unsigned opus_pcm_bits = 16, opus_complexity = 10;
uint32_t codec_api_cycles;

const char *codec_name(void)
{
#if defined(FIXED_POINT) && defined(ENABLE_RES24)
    return "Opus-1.6.1-CELT-fixed-res24";
#elif defined(FIXED_POINT)
    return "Opus-1.6.1-CELT-fixed-res16";
#else
    return "Opus-1.6.1-CELT-float";
#endif
}

int codec_init(unsigned frame_us, unsigned bitrate)
{
    if (frame_us < 2500) return -ENOTSUP;
#if defined(FIXED_POINT) && !defined(ENABLE_RES24)
    /* Do not present the default fixed-point API's 24-to-16 conversion as
     * true 24-bit processing. That is measured in the RES24 build. */
    if (opus_pcm_bits == 24) return -ENOTSUP;
#endif
    samples = frame_us * 48 / 1000;
    packet_bytes = (uint64_t)bitrate * frame_us / 8000000;
    rng = 0x48125340; /* Identical source sequence for every precision/complexity. */
    for (unsigned ch=0; ch<2; ch++) for (unsigned i=0; i<480; i++)
        tone[ch][i] = (int16_t)(12000.f * sinf(6.283185307f * (ch ? 17 : 10) * i / 480.f));
    int size = opus_encoder_get_size(2);
    if (size <= 0 || size > sizeof(state)) return -ENOMEM;
    int rc = opus_encoder_init(enc, 48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY);
#define SET(x) do { if (!rc) rc = opus_encoder_ctl(enc, x); } while (0)
    SET(OPUS_SET_BITRATE(bitrate));
    SET(OPUS_SET_VBR(0));
    SET(OPUS_SET_COMPLEXITY(opus_complexity));
    SET(OPUS_SET_FORCE_CHANNELS(2));
    SET(OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    SET(OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    SET(OPUS_SET_INBAND_FEC(0));
    SET(OPUS_SET_DTX(0));
    SET(OPUS_SET_PACKET_LOSS_PERC(0));
    SET(OPUS_SET_LSB_DEPTH(opus_pcm_bits));
    int lookahead=0;
    SET(OPUS_GET_LOOKAHEAD(&lookahead));
#undef SET
    printk("OPUS_CONFIG pcm_bits=%u complexity=%u cbr=1 stereo=1 bandwidth=fullband lookahead_samples=%d state=%d init=%d\n",
           opus_pcm_bits, opus_complexity, lookahead, size, rc);
    return rc;
}

int codec_encode(unsigned sequence, uint8_t *out, unsigned *bytes)
{
    /* Same four content classes as the LC3plus test. 24-bit input has genuine
     * nonzero low bits; the 16-bit signal is its quantized counterpart. */
    for (unsigned ch=0; ch<2; ch++) for (unsigned i=0; i<samples; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        unsigned phase = (sequence / 25) % 4;
        int16_t x = tone[ch][(sequence * samples + i) % 480];
        x = phase == 0 ? x : phase == 1 ? (int16_t)rng / 2 : phase == 2 ? 0 : ((sequence+i)%7 == 0 ? (int16_t)rng : x);
        pcm16[2*i+ch] = x;
        pcm24[2*i+ch] = (int32_t)x * 256 + (phase == 2 ? 0 : (rng & 255));
    }
    uint32_t start = DWT->CYCCNT;
    int n = opus_pcm_bits == 24 ? opus_encode24(enc, pcm24, samples, out, MAX_FRAME)
                               : opus_encode(enc, pcm16, samples, out, MAX_FRAME);
    codec_api_cycles = DWT->CYCCNT - start;
    *bytes = n > 0 ? n : 0;
    if (n < 0) return n;
    /* Validate constant bitrate, CELT ToC, stereo, duration and full bandwidth. */
    if (n != packet_bytes || !(out[0] & 0x80) || opus_packet_get_nb_channels(out) != 2 ||
        opus_packet_get_nb_samples(out, n, 48000) != samples ||
        opus_packet_get_bandwidth(out) != OPUS_BANDWIDTH_FULLBAND) return -EBADMSG;
    return 0;
}
