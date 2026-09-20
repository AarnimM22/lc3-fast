#pragma once
#include <lc3.h>
#include "lc3_lite.h"
/* Research format v1: one independently parseable 2-byte header per channel.
 * Not LC3/LC3plus wire compatible. Only 48 kHz, 2.5 ms standard mode is tested.
 * The benchmark is single-encoder-thread; these controls are not reentrant. */
enum { CUSTOM_SNS_STOCK, CUSTOM_SNS_COARSE, CUSTOM_SNS_UNITY, CUSTOM_SNS_SCALAR };
enum { CUSTOM_CBR, CUSTOM_VBR_SEARCH, CUSTOM_VBR_ANALYTIC };
#define CUSTOM_MAX_CHANNEL 102
extern unsigned custom_sns, custom_rate, custom_tns;
extern unsigned custom_nbytes, custom_cap_frames, custom_sns_clips;
void lc3_custom_configure(unsigned sns, unsigned rate, unsigned tns);
int lc3_custom_encode(lc3_encoder_t enc, enum lc3_pcm_format fmt,
    const void *pcm, int stride, int target_bytes, void *out, int capacity);
int lc3_custom_decode(lc3_decoder_t dec, const void *packet, int length,
    enum lc3_pcm_format fmt, void *pcm, int stride);
