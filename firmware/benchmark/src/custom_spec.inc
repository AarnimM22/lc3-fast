/* A bounded analytic approximation to upstream's piecewise bit-cost model.
 * Two slope corrections replace the eight-step binary gain search. This is
 * deliberately an approximation, not a perceptual-error guarantee. */
static int custom_estimate_gain(enum lc3_dt dt, enum lc3_srate sr,
    const float *x, float budget, int g_off)
{
    float e[LC3_MAX_NE/4], sum=0, max2=0;
    int n4=lc3_ne(dt,sr)/4;
    for (int i=0;i<n4;i++) {
        float s=0;
        for (int j=0;j<4;j++) {
            float v=x[4*i+j]*x[4*i+j];
            s+=v; max2=fmaxf(max2,v);
        }
        e[i]=lc3_db_q16(fmaxf(s,1e-10f))*(1.f/65536);
        sum+=e[i];
    }
    float gn=(sum+7*n4-1.4f*budget)/n4;
    for (int pass=0;pass<2;pass++) {
        int last=n4-1;
        while (last>=0 && e[last]<gn) last--;
        float cost=0, slope=0;
        for (int i=0;i<=last;i++) {
            float d=e[i]-gn;
            if (d<0) cost+=2.7f;
            else if (d<43) { cost+=d+7; slope+=1; }
            else { cost+=2*d-36; slope+=2; }
        }
        gn+=(cost-1.4f*budget)/fmaxf(slope,1);
    }
    int gain=(int)ceilf(1.4f*gn);
    /* Quantizer safety bound; signal the selected gain in every frame. */
    int minimum=max2>0 ? (int)ceilf(14*lc3_log10f(max2/(32767.f*32767.f))) : -g_off;
    return LC3_CLIP(LC3_MAX(gain,minimum),-g_off,255-g_off);
}

static void custom_spec_analyze(enum lc3_dt dt, enum lc3_srate sr, int nbytes,
    bool pitch, const lc3_tns_data_t *tns, struct lc3_spec_analysis *spec,
    float *x, struct lc3_spec_side *side)
{
    /* This research configuration confines all payloads to 20..100 bytes,
     * within one entropy-context regime; TNS weighting is also unchanged. */
    int overhead=get_nbits_ac(dt,sr,nbytes)+lc3_bwdet_get_nbits(sr)+
        lc3_ltpf_get_nbits(pitch)+lc3_sns_get_nbits()+lc3_tns_get_nbits(tns)+11;
    int budget=8*nbytes-overhead;
    int g_off=resolve_gain_offset(sr,nbytes), g;
    if (custom_rate == CUSTOM_VBR_SEARCH) {
        bool reset; int g_min;
        LITE_MEASURE(PROF_GAIN, g=estimate_gain(dt,sr,x,nbytes,budget,
            spec->nbits_off,g_off,&reset,&g_min));
    } else {
        LITE_MEASURE(PROF_GAIN, g=custom_estimate_gain(dt,sr,x,
            fmaxf(16,budget+spec->nbits_off),g_off));
    }
    g=LC3_CLIP(g,-resolve_gain_offset(sr,20),255-resolve_gain_offset(sr,100));
    quantize(dt,sr,g,x,&side->nq);
    int full_nq=side->nq;
    int bits=compute_nbits(dt,sr,nbytes,x,&side->nq,0,NULL);
    /* Reserve one residual bit per potentially coded coefficient, plus a byte
     * of termination margin. Cap the burst reservoir at two nominal frames. */
    int wanted=(bits+overhead+side->nq+8+7)/8;
    /* Bound the burst to eight extra bytes/channel as well as the reservoir:
     * arithmetic coding work and retry airtime matter along with mean rate. */
    int cap=LC3_CLIP(nbytes+LC3_MIN(spec->nbits_spare,8),20,100);
    int actual=LC3_CLIP(wanted,20,cap);
    if (wanted>cap) {
        custom_cap_frames++;
        /* One bounded correction, never an encode/re-encode loop. Remaining
         * excess is truncated by the inherited hard-budget code below. */
        int delta=LC3_MAX(1,(int)ceilf(1.4f*8*(wanted-cap)/(lc3_ne(dt,sr)/4)));
        delta=LC3_MIN(delta,255-resolve_gain_offset(sr,actual)-g);
        if (delta>0) { quantize(dt,sr,delta,x,&side->nq); g+=delta; }
    }
    /* wanted reserves the first pass's entire cost plus residual bits and a
     * termination byte. With unchanged quantization/context and no LSB mode,
     * the second pass cannot truncate anything. Reuse its existing endpoint.
     * Capped/requantized frames retain the original exact budget walk. */
#if OPT_SPEC
    bool old_lsb, new_lsb;
    bool old_high=resolve_modes(sr,nbytes,&old_lsb);
    bool new_high=resolve_modes(sr,actual,&new_lsb);
    if (wanted<=cap && !old_lsb && !new_lsb && old_high==new_high) {
        side->lsb_mode=false;
    } else
#endif
    {
        side->nq=full_nq;
        compute_nbits(dt,sr,actual,x,&side->nq,8*actual-overhead,&side->lsb_mode);
    }
    side->g_idx=g+resolve_gain_offset(sr,actual);
    custom_nbytes=actual;
    spec->nbits_spare=LC3_CLIP(spec->nbits_spare+nbytes-actual,0,2*nbytes);
    /* Feedback must see the requested length, not just the capped output.
     * Otherwise a capped frame equal to target never updates the estimate. */
    int demand=LC3_MIN(wanted,2*nbytes);
    spec->nbits_off=LC3_CLIP(spec->nbits_off+0.10f*8*(nbytes-demand),-budget/2.f,budget/2.f);
}
