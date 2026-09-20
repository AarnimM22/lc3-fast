#include "bench.h"
#include <sbc.h>
#include <cmsis_core.h>
#include <math.h>

static sbc_t encoder;
static struct sbc_frame format;
static int16_t pcm[256], tones[2][480];
static uint32_t rng, rate;
uint32_t codec_api_cycles;

const char *codec_name(void) { return "Google-SBC-XQ-dual-16"; }

int codec_init(unsigned frame_us, unsigned bitrate)
{
    if (frame_us != 2667 || (bitrate != 368000 && bitrate != 372000) || BENCH_PCM_BITS != 16)
        return -EINVAL;
    rate = bitrate;
    rng = 0x48125340;
    sbc_reset(&encoder);
    format = (struct sbc_frame){.freq=SBC_FREQ_48K, .mode=SBC_MODE_DUAL_CHANNEL,
        .bam=SBC_BAM_LOUDNESS, .nblocks=16, .nsubbands=8, .bitpool=28};
    for (unsigned ch=0;ch<2;ch++) for(unsigned i=0;i<480;i++)
        tones[ch][i] = (int16_t)(12000.f*sinf(6.283185307f*(ch?17:10)*i/480.f));
    printk("SBC_CONFIG pcm_bits=16 samples=128 period_num_us=8000 period_den=3 mode=dual subbands=8 blocks=16 bitpool_min=%u bitpool_max=28 rate=%u\n",
        rate==368000?27:28,rate);
    return 0;
}

int codec_encode(unsigned sequence, uint8_t *out, unsigned *bytes)
{
    for(unsigned ch=0;ch<2;ch++) for(unsigned i=0;i<128;i++) {
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
        unsigned phase=(sequence/25)%4;
        int16_t x=tones[ch][(sequence*128+i)%480];
        pcm[2*i+ch]=phase==0?x:phase==1?(int16_t)rng/2:phase==2?0:((sequence+i)%7==0?(int16_t)rng:x);
    }
    format.bitpool=rate==368000 && sequence%3==0 ? 27:28;
    *bytes=sbc_get_frame_size(&format);
    uint32_t start=DWT->CYCCNT;
    int rc=sbc_encode(&encoder,pcm,2,pcm+1,2,&format,out,MAX_FRAME);
    codec_api_cycles=DWT->CYCCNT-start;
    struct sbc_frame parsed;
    if (rc || sbc_probe(out,&parsed) || parsed.bitpool!=format.bitpool ||
        parsed.mode!=format.mode || parsed.nblocks!=16 || parsed.nsubbands!=8 ||
        parsed.freq!=SBC_FREQ_48K || *bytes!=(format.bitpool==27?120:124)) return -EBADMSG;
    return 0;
}
