/* Included in the generated sns.c to reuse its interpolation and optional
 * original scale-factor estimator. Four-bit values encode 0.75-octave steps.
 * Mean removal makes absolute level the spectral quantizer's responsibility. */
static void custom_shape_apply(const struct lc3_sns_data *data, bool inv,
    const float *x, float *y)
{
    const int *lim=lc3_band_lim[LC3_DT_2M5][LC3_SRATE_48K];
    for (int b=0,k=0;b<44;b++) {
        const int8_t *w=custom_band_weights[b];
        int grid=w[2]*((int)data->coarse[w[0]]-8)+w[3]*((int)data->coarse[w[1]]-8);
        float gain=custom_shape_gain[(inv ? grid : -grid)+224];
        for (;k<lim[b+1];k++) y[k]=x[k]*gain;
    }
}

static void custom_sns_analyze(enum lc3_dt dt, enum lc3_srate sr, int nbytes,
    const float *eb, bool att, struct lc3_sns_data *data,
    const float *x, float *y)
{
    if (custom_sns == CUSTOM_SNS_UNITY) {
        if (x != y) memcpy(y,x,lc3_ne(dt,sr)*sizeof(*y));
        return;
    }
    float scf[16];
    if (custom_sns == CUSTOM_SNS_SCALAR) {
        compute_scale_factors(dt,sr,nbytes,eb,att,scf);
    } else {
        /* Same 64-position energy padding as upstream, then four-bin groups.
         * The 48 kHz tilt is evaluated once per group instead of per band. */
        float e[16], mean=0, sum=0;
#if OPT_SNS
        /* The 44-band mode duplicates bands 0..19, then copies 20..43.
         * Preserve the four-addend expression and grouping of the old path. */
        if (dt==LC3_DT_2M5 && sr==LC3_SRATE_48K) {
            for (int i=0;i<10;i++) {
                int j=2*i;
                e[i]=0.25f*(eb[j]+eb[j]+eb[j+1]+eb[j+1]);
            }
            for (int i=10;i<16;i++) {
                int j=20+4*(i-10);
                e[i]=0.25f*(eb[j]+eb[j+1]+eb[j+2]+eb[j+3]);
            }
            for (int i=0;i<16;i++) {
                e[i]*=custom_tilt[i]; sum+=e[i];
            }
        } else
#endif
        {
        float padded[64];
        int nb=lc3_num_bands[dt][sr];
        int n4=nb<32 ? 32%nb : 0;
        int n2=nb<32 ? nb-n4 : 64-nb;
        for (int i=0;i<n4;i++)
            for (int j=0;j<4;j++) padded[4*i+j]=eb[i];
        for (int i=n4;i<n4+n2;i++)
            padded[2*(n4+i)]=padded[2*(n4+i)+1]=eb[i];
        memcpy(padded+4*n4+2*n2,eb+n4+n2,(nb-n4-n2)*sizeof(float));
        for (int i=0;i<16;i++) {
            e[i]=0.25f*(padded[4*i]+padded[4*i+1]+padded[4*i+2]+padded[4*i+3]);
            e[i]*=custom_tilt[i];
            sum+=e[i];
        }
        }
        float floor=fmaxf(sum*(1e-4f/16),0x1p-32f);
        for (int i=0;i<16;i++) {
            scf[i]=0.5f*lc3_log2f(fmaxf(e[i],floor));
            mean+=scf[i];
        }
        for (int i=0;i<16;i++) scf[i]=0.85f*(scf[i]-mean/16);
    }
    for (int i=0;i<16;i++) {
        float q=scf[i]*(1.f/0.75f)+8;
        custom_sns_clips+=(q<-.5f || q>=15.5f);
        int code=(int)LC3_CLIP(q+0.5f,0,15);
        data->coarse[i]=code;
    }
    custom_shape_apply(data,false,x,y);
}
