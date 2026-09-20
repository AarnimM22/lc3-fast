#include "bench.h"
#include <cmsis_core.h>
#include <math.h>
#include <string.h>
#include <wavpack-stream.h>

static WavpackContext *encoder;
static unsigned samples, produced, blocks;
static uint8_t *output;
static int32_t pcm[480*2];
static int16_t tone[2][480];
static uint32_t rng;
uint32_t codec_api_cycles;
unsigned wavpack_fast;

static int block_output(void *id, void *data, int32_t count)
{
    ARG_UNUSED(id);
    if (count < 12 || produced+(unsigned)count > MAX_FRAME) return 0;
    memcpy(output+produced, data, count);
    produced+=count;
    blocks++;
    return 1;
}

const char *codec_name(void) { return "WavPack-stream-79ec9e1"; }

int codec_init(unsigned frame_us, unsigned bitrate)
{
    if (encoder) encoder=WavpackStreamCloseFile(encoder);
    samples=frame_us*48/1000;
    if (samples<50 || samples>480) return -EINVAL;
    rng=0x48125340;
    for (unsigned ch=0;ch<2;ch++) for (unsigned i=0;i<480;i++)
        tone[ch][i]=(int16_t)(12000.f*sinf(6.283185307f*(ch?17:10)*i/480.f));
    encoder=WavpackStreamOpenFileOutput(block_output, NULL, NULL);
    if (!encoder) return -ENOMEM;
    WavpackStreamConfig config={0};
    config.sample_rate=48000;
    config.num_channels=2;
    config.channel_mask=3;
    config.bits_per_sample=BENCH_PCM_BITS;
    config.bytes_per_sample=BENCH_PCM_BITS/8;
    config.block_samples=samples;
    config.bitrate=bitrate/1000.f;
    config.flags=CONFIG_HYBRID_FLAG|CONFIG_BITRATE_KBPS|(wavpack_fast?CONFIG_FAST_FLAG:0);
    if (!WavpackStreamSetConfiguration64(encoder,&config,-1,NULL) ||
        !WavpackStreamPackInit(encoder)) {
        printk("WAVPACK_INIT_ERROR %s\n",WavpackStreamGetErrorMessage(encoder));
        return -EINVAL;
    }
    printk("WAVPACK_CONFIG samples=%u bits=%u target_kbps=%u fast=%u correction=0\n",
           samples,BENCH_PCM_BITS,bitrate/1000,wavpack_fast);
    return 0;
}

int codec_encode(unsigned sequence, uint8_t *out, unsigned *bytes)
{
    for (unsigned ch=0;ch<2;ch++) for (unsigned i=0;i<samples;i++) {
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
        unsigned phase=(sequence/25)%4;
        int16_t x=tone[ch][(sequence*samples+i)%480];
        x=phase==0?x:phase==1?(int16_t)rng/2:phase==2?0:
            ((sequence+i)%7==0?(int16_t)rng:x);
        pcm[i*2+ch]=BENCH_PCM_BITS==24 ? (int32_t)x*256+(phase==2?0:(rng&255)) : x;
    }
    output=out; produced=blocks=0;
    uint32_t start=DWT->CYCCNT;
    int ok=WavpackStreamPackSamples(encoder,pcm,samples);
    codec_api_cycles=DWT->CYCCNT-start;
    *bytes=produced;
    /* Full fixed-size input blocks must emit immediately, without flushing
     * or resetting predictors between blocks. Include all codec metadata. */
    if (!ok || blocks!=1 || !produced) return -EIO;
    return 0;
}
