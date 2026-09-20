#include "lc3_custom.h"
#include <stddef.h>
unsigned custom_sns, custom_rate, custom_tns;
unsigned custom_nbytes, custom_cap_frames, custom_sns_clips;
void lc3_custom_configure(unsigned sns, unsigned rate, unsigned tns)
{
    custom_sns=sns; custom_rate=rate; custom_tns=tns;
    custom_nbytes=custom_cap_frames=custom_sns_clips=0;
    lc3_lite_set_flags(LITE_NO_LTPF | (tns ? 0 : LITE_NO_TNS));
}
int lc3_custom_encode(lc3_encoder_t enc, enum lc3_pcm_format fmt,
    const void *pcm, int stride, int target_bytes, void *out, int capacity)
{
    if (!enc || !pcm || !out || stride<1 || (unsigned)fmt>LC3_PCM_FORMAT_FLOAT ||
        target_bytes<22 || target_bytes>CUSTOM_MAX_CHANNEL ||
        capacity<CUSTOM_MAX_CHANNEL || custom_sns>3 || custom_rate>2 ||
        enc->dt!=LC3_DT_2M5 || enc->sr!=LC3_SRATE_48K) return -1;
    uint8_t *p=out;
    lc3_encoder_disable_ltpf(enc);
    custom_nbytes=target_bytes-2;
    int rc=lc3_encode(enc,fmt,pcm,stride,target_bytes-2,p+2);
    if (rc) return rc;
    p[0]=0xd0|custom_sns;
    p[1]=custom_nbytes;
    return custom_nbytes+2;
}
int lc3_custom_decode(lc3_decoder_t dec, const void *packet, int length,
    enum lc3_pcm_format fmt, void *pcm, int stride)
{
    if (!dec || !pcm || stride<1 || (unsigned)fmt>LC3_PCM_FORMAT_FLOAT ||
        dec->dt!=LC3_DT_2M5 || dec->sr!=LC3_SRATE_48K) return -1;
    if (!packet) return lc3_decode(dec,NULL,20,fmt,pcm,stride);
    const uint8_t *p=packet;
    if (length<22 || length>CUSTOM_MAX_CHANNEL || (p[0]&0xfc)!=0xd0 ||
        p[1]!=length-2) return -1;
    unsigned saved=custom_sns;
    custom_sns=p[0]&3;
    int rc=lc3_decode(dec,p+2,p[1],fmt,pcm,stride);
    custom_sns=saved;
    return rc;
}
