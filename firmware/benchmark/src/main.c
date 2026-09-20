#include "bench.h"
#include <string.h>
#include <zephyr/sys/crc.h>
#include <cmsis_core.h>
#ifdef BENCH_RX
#include "rx_ipc.h"
#endif
#ifdef CODEC_LC3_LITE
#include "lc3_lite.h"
#endif
#ifdef CODEC_LC3_CUSTOM
#include "lc3_custom.h"
#endif
#ifdef BENCH_TX
static uint8_t encoded[MAX_FRAME];
static uint32_t timings[1024];
#ifdef CODEC_LC3_CUSTOM
static uint8_t vector_data[4][204];
static unsigned vector_bytes[4];
#endif
static uint32_t cycles_to_us(uint64_t cycles)
{
    return (uint32_t)(cycles * 1000000u / SystemCoreClock);
}
static int send_frame(uint32_t run, uint32_t seq, unsigned bytes,
                      bool async, uint64_t ready_us, uint64_t deadline_us)
{
    uint32_t crc = crc32_ieee(encoded, bytes);
    for (unsigned off=0; off<bytes; off+=CHUNK) {
        uint8_t packet[252];
        struct header h = {MAGIC, run, seq, crc, bytes, off, DATA, {0}};
        unsigned n = MIN(CHUNK, bytes-off);
        memcpy(packet, &h, sizeof(h)); memcpy(packet+sizeof(h), encoded+off, n);
        int rc;
#ifdef BENCH_HAS_ASYNC
        if (async) rc = radio_async_submit(packet, sizeof(h)+n, ready_us, deadline_us);
        else
#endif
            rc = radio_send(packet, sizeof(h)+n);
        if (rc) return rc;
    }
    return 0;
}
static void control(unsigned type, uint32_t run, uint32_t count)
{
    struct header h = {.magic=MAGIC, .run=run, .seq=count, .type=type};
    for (int i=0;i<3;i++) { if (!radio_send(&h, sizeof(h))) return; }
    printk("CONTROL_FAIL run=%u type=%u\n", run, type);
}
static void run_case(unsigned id, unsigned mode, unsigned frame_us, unsigned bitrate, unsigned nframes)
{
    /* mode 0: local encoding, 1: sequential RF, 2: synthetic RF only,
     * mode 3: encoding with concurrent RF at a fixed source cadence. */
    if (mode != 2) {
        int rc = codec_init(frame_us, bitrate);
        if (rc) { printk("CONFIG_ERROR id=%u rc=%d\n", id, rc); return; }
    }
    printk("BEGIN id=%u mode=%u frame_us=%u bitrate=%u frames=%u\n", id, mode, frame_us, bitrate, nframes);
    if (mode) control(START, id, nframes);
#ifdef BENCH_HAS_ASYNC
    struct radio_async_stats async_stats = {0};
    if (mode == 3 && radio_async_reset()) {
        printk("ASYNC_RESET_ERROR id=%u\n", id);
        return;
    }
#endif
    uint32_t ok0=tx_ok, fail0=tx_fail, ret0=tx_retries;
    uint64_t cycles=0, work=0, start=now_us(), due=start;
    uint64_t payload_bytes=0, slots=0;
    uint32_t payload_min=UINT32_MAX, payload_max=0, fragmented=0, data_packets=0;
#ifdef CODEC_SBC
    const unsigned period_num=8000, period_den=3;
#else
    const unsigned period_num=frame_us, period_den=1;
#endif
    uint32_t worst=0, worst_work=0, late=0, errors=0, send_errors=0, skips=0, timing_errors=0;
    uint64_t api_cycles=0;
    uint32_t api_max=0;
    uint32_t release_late_max=0;
    for (unsigned seq=0; seq<nframes; seq++) {
        uint64_t release=start+(uint64_t)seq*period_num/period_den;
        if (mode == 3) {
            uint64_t now=now_us();
            if (now<release) k_sleep(K_USEC(release-now));
            now=now_us();
            release_late_max=MAX(release_late_max,now>release ? now-release : 0);
        }
        unsigned bytes = bitrate*(uint64_t)frame_us/8000000;
        uint64_t t0=now_us();
        /* J-Link may clear trace enable on detach. Re-arm each measurement and
         * reject a case if a mid-frame probe change invalidates its counter. */
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        uint32_t c0=DWT->CYCCNT;
        if (mode != 2) {
            if (codec_encode(seq, encoded, &bytes)) errors++;
        } else {
            for (unsigned i=0;i<bytes;i++) encoded[i]=(uint8_t)(seq+i*29);
        }
        uint32_t dc=DWT->CYCCNT-c0;
        uint32_t measured_us=now_us()-t0;
        uint32_t cycle_us=cycles_to_us(dc);
        if (!dc || cycle_us+200<measured_us || measured_us+200<cycle_us) timing_errors++;
        if (mode != 2) { api_cycles+=codec_api_cycles; api_max=MAX(api_max,codec_api_cycles); }
        cycles+=dc; worst=MAX(worst, dc);
        payload_bytes+=bytes;
        payload_min=MIN(payload_min,bytes); payload_max=MAX(payload_max,bytes);
        fragmented+=bytes>CHUNK;
        data_packets+=DIV_ROUND_UP(bytes,CHUNK);
        if (seq<ARRAY_SIZE(timings)) timings[seq]=dc;
        /* Async ACK target: one further frame interval after the encode
         * deadline. Track its violations separately from producer cadence. */
        if (mode && send_frame(id,seq,bytes,mode==3,release,
                               release+2*period_num/period_den)) send_errors++;
#ifdef CODEC_LC3_CUSTOM
        if (seq<=75 && seq%25==0 && bytes<=204) {
            vector_bytes[seq/25]=bytes;
            memcpy(vector_data[seq/25],encoded,bytes);
        }
#endif
        uint32_t elapsed=now_us()-t0;
        work+=elapsed; worst_work=MAX(worst_work,elapsed);
        if (mode == 3) {
            due=start+(uint64_t)(seq+1)*period_num/period_den;
            if (now_us()>due) late++;
            /* Never shift the source phase or skip input to hide overload. */
        } else if (mode) {
            due=start+(++slots)*period_num/period_den;
            uint64_t now=now_us();
            if (now>due) {
                late++;
                /* Account for unsustainable source cadence; do not hide it
                 * by moving the next deadline forward without reporting. */
                while (start+(slots+1)*period_num/period_den<=now) { slots++; skips++; }
            } else k_sleep(K_USEC(due-now));
        }
    }
    uint64_t duration=now_us()-start;
#ifdef BENCH_HAS_ASYNC
    if (mode == 3) {
        int rc=radio_async_drain(&async_stats);
        if (rc) { printk("ASYNC_DRAIN_ERROR id=%u rc=%d\n",id,rc); return; }
        send_errors+=async_stats.errors;
        /* Include the last ACK; compare duration against nframes*period. */
        duration=now_us()-start;
    }
#endif
    if (mode) { control(END,id,nframes); k_sleep(K_MSEC(150)); }
    unsigned tcount=MIN(nframes, ARRAY_SIZE(timings));
    for(unsigned i=1;i<tcount;i++) { uint32_t x=timings[i]; unsigned j=i; while(j&&timings[j-1]>x){timings[j]=timings[j-1];j--;} timings[j]=x; }
    printk("RESULT id=%u mode=%u frame_us=%u bitrate=%u frames=%u enc_avg_us=%u enc_max_us=%u enc_p99_us=%u work_avg_us=%u work_max_us=%u late=%u skipped=%u codec_errors=%u send_errors=%u ack_ok=%u ack_fail=%u retries=%u elapsed_us=%llu",
       id,mode,frame_us,bitrate,nframes,cycles_to_us(cycles/nframes),cycles_to_us(worst),cycles_to_us(timings[(tcount-1)*99/100]),(uint32_t)(work/nframes),worst_work,late,skips,errors,send_errors,tx_ok-ok0,tx_fail-fail0,tx_retries-ret0,duration);
#ifdef CODEC_OPUS
    printk(" pcm_bits=%u complexity=%u",opus_pcm_bits,opus_complexity);
#else
    printk(" pcm_bits=%u",BENCH_PCM_BITS);
#endif
    printk(" api_avg_us=%u api_max_us=%u",cycles_to_us(api_cycles/nframes),cycles_to_us(api_max));
#ifdef CODEC_LC3_LITE
    printk(" lite_flags=%u",lc3_lite_flags);
#endif
#ifdef CODEC_LC3_CUSTOM
    printk(" sns_mode=%u rate_mode=%u tns=%u cap_channels=%u sns_clips=%u",
        custom_sns,custom_rate,custom_tns,custom_cap_frames,custom_sns_clips);
#endif
#ifdef CODEC_WAVPACK
    printk(" wavpack_fast=%u",wavpack_fast);
#endif
    printk(" timing_errors=%u payload_bytes=%llu period_num_us=%u period_den=%u",timing_errors,payload_bytes,period_num,period_den);
#ifdef BENCH_HAS_ASYNC
    if (mode == 3) printk(" radio_completed=%u radio_late=%u pending_peak=%u radio_service_avg_us=%u radio_service_max_us=%u ready_to_ack_avg_us=%u ready_to_ack_max_us=%u release_late_max_us=%u ack_budget_us=%u",
        async_stats.completed,async_stats.late,async_stats.peak_pending,
        (uint32_t)(async_stats.service_total_us/MAX(async_stats.completed,1)),async_stats.service_max_us,
        (uint32_t)(async_stats.latency_total_us/MAX(async_stats.completed,1)),async_stats.latency_max_us,
        release_late_max,2*period_num/period_den);
#endif
    printk(" payload_min=%u payload_max=%u fragmented_frames=%u data_packets=%u\n",payload_min,payload_max,fragmented,data_packets);
#ifdef CODEC_LC3_LITE
    /* Gain is nested inside spec; do not add it again to the stage total. */
    printk("PROFILE id=%u flags=%u",id,lc3_lite_flags);
    const char *names[] = {"attack","ltpf","mdct","energy","bw","sns","tns","spec","bits","gain"};
    for (unsigned i=0;i<PROF_COUNT;i++)
        printk(" %s_us=%u",names[i],cycles_to_us(lc3_lite_cycles[i]/nframes));
    printk("\n");
#endif
#ifdef CODEC_LC3_CUSTOM
    for (unsigned v=0;v<4 && v*25<nframes;v++) {
        printk("VECTOR id=%u seq=%u bytes=%u data=",id,v*25,vector_bytes[v]);
        for (unsigned b=0;b<vector_bytes[v];b++) printk("%02x",vector_data[v][b]);
        printk("\n");
    }
#endif
}
int main(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT=0; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    int rc=radio_init();
    printk("BOOT TX codec=%s lto=%u cpu_hz=%u radio_init=%d frames=%u\n",codec_name(),IS_ENABLED(CONFIG_LTO),SystemCoreClock,rc,BENCH_FRAMES);
    if(rc) return rc;
    k_sleep(K_SECONDS(10));
    unsigned id=0;
#ifdef BENCH_NRF54
    const unsigned rates[] = {256000,320000};
#if BENCH_ASYNC_ONLY
    id=100;
    for (unsigned r=0;r<ARRAY_SIZE(rates);r++)
        run_case(++id,3,2500,rates[r],BENCH_FRAMES);
    if (BENCH_SOAK_FRAMES)
        for (unsigned r=0;r<ARRAY_SIZE(rates);r++)
            run_case(++id,3,2500,rates[r],BENCH_SOAK_FRAMES);
#else
    const unsigned frame_periods[] = {10000,5000,2500};
    for (unsigned r=0;r<ARRAY_SIZE(rates);r++)
        for (unsigned mode=0;mode<=1;mode++)
            for (unsigned f=0;f<ARRAY_SIZE(frame_periods);f++)
                run_case(++id,mode,frame_periods[f],rates[r],BENCH_FRAMES);
    if (BENCH_SOAK_FRAMES)
        for (unsigned r=0;r<ARRAY_SIZE(rates);r++)
            run_case(++id,1,2500,rates[r],BENCH_SOAK_FRAMES);
#endif
#elif defined(CODEC_LC3_CUSTOM)
    const unsigned rates[] = {256000,320000,400000,480000};
    const unsigned configs[][3] = {
        {0,0,1}, {1,0,1}, {2,0,1}, {3,0,1},
        {1,1,1}, {1,2,1}, {2,2,1}, {3,2,1},
        {1,0,0}, {1,2,0}, {3,0,0}, {3,2,0}
    };
    id=700;
#if !BENCH_ASYNC_ONLY
    for (unsigned c=0;c<ARRAY_SIZE(configs);c++)
        for (unsigned mode=0;mode<=3;mode+=3)
            for (unsigned r=0;r<ARRAY_SIZE(rates);r++) {
                lc3_custom_configure(configs[c][0],configs[c][1],configs[c][2]);
                run_case(++id,mode,2500,rates[r],BENCH_FRAMES);
            }
#endif
    if (BENCH_SOAK_FRAMES) {
        id=800;
        for (unsigned r=1;r<3;r++) {
            lc3_custom_configure(CUSTOM_SNS_COARSE,CUSTOM_VBR_ANALYTIC,0);
            run_case(++id,3,2500,rates[r],BENCH_SOAK_FRAMES);
        }
    }
#elif defined(CODEC_LC3_LITE)
    const unsigned rates[] = {256000,320000};
    const unsigned flags[] = {0,1,2,3,4,5,7};
    id=300;
    for (unsigned f=0;f<ARRAY_SIZE(flags);f++)
        for (unsigned mode=0;mode<=3;mode+=3)
            for (unsigned r=0;r<ARRAY_SIZE(rates);r++) {
                lc3_lite_set_flags(flags[f]);
                run_case(++id,mode,2500,rates[r],BENCH_FRAMES);
            }
    if (BENCH_SOAK_FRAMES)
        for (unsigned r=0;r<ARRAY_SIZE(rates);r++) {
            lc3_lite_set_flags(LITE_NO_LTPF);
            run_case(++id,3,2500,rates[r],BENCH_SOAK_FRAMES);
        }
#elif defined(CODEC_WAVPACK)
    const unsigned frame_periods[] = {5000,2500,1250};
    const unsigned rates[] = {256000,320000,384000};
    id=200;
    for (wavpack_fast=0;wavpack_fast<=1;wavpack_fast++)
        for (unsigned mode=0;mode<=3;mode+=3)
            for (unsigned r=0;r<ARRAY_SIZE(rates);r++)
                for (unsigned f=0;f<ARRAY_SIZE(frame_periods);f++)
                    run_case(++id,mode,frame_periods[f],rates[r],BENCH_FRAMES);
    wavpack_fast=1;
    if (BENCH_SOAK_FRAMES)
        for (unsigned r=0;r<ARRAY_SIZE(rates);r++)
            run_case(++id,3,2500,rates[r],BENCH_SOAK_FRAMES);
#elif defined(CODEC_SBC)
    for (unsigned mode=0;mode<=1;mode++) {
        run_case(++id,mode,2667,368000,BENCH_FRAMES);
        run_case(++id,mode,2667,372000,BENCH_FRAMES);
    }
    if (BENCH_SOAK_FRAMES) run_case(++id,1,2667,368000,BENCH_SOAK_FRAMES);
#elif defined(CODEC_OPUS)
    const unsigned frames[] = {10000,5000,2500};
    const unsigned complexities[] = {0,5,10};
    for (unsigned c=0;c<ARRAY_SIZE(complexities);c++) {
        opus_complexity=complexities[c];
        for (unsigned bits=16;bits<=24;bits+=8) {
            opus_pcm_bits=bits;
            for (unsigned mode=0;mode<=1;mode++)
                for (unsigned f=0;f<ARRAY_SIZE(frames);f++)
                    run_case(++id,mode,frames[f],BENCH_BITRATE,BENCH_FRAMES);
        }
    }
    if (BENCH_SOAK_FRAMES) {
        opus_complexity=0;
        for (unsigned bits=16;bits<=24;bits+=8) {
            opus_pcm_bits=bits;
            run_case(++id,1,2500,BENCH_BITRATE,BENCH_SOAK_FRAMES);
        }
    }
#else
    const unsigned frames[] = {10000,5000,2500,1250};
    for(unsigned mode=0;mode<3;mode++) {
        for(unsigned i=0;i<ARRAY_SIZE(frames);i++) run_case(++id,mode,frames[i],BENCH_BITRATE,BENCH_FRAMES);
        if (mode!=0 && BENCH_BITRATE==320000) run_case(++id,mode,5000,160000,BENCH_FRAMES);
        if (mode!=0 && BENCH_BITRATE==320000) run_case(++id,mode,5000,240000,BENCH_FRAMES);
    }
    if (BENCH_SOAK_FRAMES) {
        run_case(++id,2,2500,BENCH_BITRATE,BENCH_SOAK_FRAMES);
        run_case(++id,2,1250,BENCH_BITRATE,BENCH_SOAK_FRAMES);
    }
#endif
    printk("SUITE_DONE\n");
    while(1) { k_sleep(K_SECONDS(10)); printk("IDLE suite_done=1\n"); }
}
#else
static uint8_t frame[MAX_FRAME];
int main(void)
{
    int rc=radio_init();
    int ipc=rx_ipc_init();
    printk("BOOT RX radio_init=%d ipc=%d channel=40 rate=2M ack=1\n",rc,ipc);
    if(rc) return rc;
    uint32_t run=0, good=0, bad=0, packets=0, expected=0, seq=UINT32_MAX, crc=0;
    uint32_t ipc_send_errors=0;
    unsigned have=0, total=0;
    bool active=false;
    uint64_t start=0, last=now_us();
    while(1) {
        struct rx_packet p;
        if (k_msgq_get(&rx_queue,&p,K_SECONDS(2))) {
            printk("RX_ALIVE run=%u active=%u good=%u bad=%u overflow=%u\n",run,active,good,bad,rx_overflow);
            continue;
        }
        struct header h;
        if (p.length<sizeof(h)) continue;
        memcpy(&h,p.data,sizeof(h));
        if(h.magic!=MAGIC) continue;
        if(h.type==START) {
            if(active&&h.run==run) continue;
            run=h.run; good=bad=packets=ipc_send_errors=0; expected=h.seq;
            seq=UINT32_MAX; have=total=0; active=true; start=now_us();
            ipc_send_errors += rx_ipc_send_start(run, expected) != 0;
            printk("RX_BEGIN run=%u expected=%u\n",run,expected);
        } else if(h.type==END && h.run==run && active) {
            ipc_send_errors += rx_ipc_send_end(run, expected) != 0;
            struct rx_app_status app={0};
            int app_wait=rx_ipc_wait_status(&app,2000);
            printk("RX_SERVO run=%u updates=%u steps=%d error_us=%d peak_error_us=%d rejected=%u bias=%d\n",
                run,app.clock_updates,app.clock_steps,app.clock_error_us,
                app.clock_peak_error_us,app.clock_rejected,app.clock_test_bias);
            printk("RX_CLOCK run=%u dma_released=%u dma_span_us=%u dma_gap_max_us=%u arrival_span_us=%u pcm_slab_early=%u pcm_remaining=%u dac_irqs=%u\n",
                run,app.dma_released,app.dma_span_us,app.dma_gap_max_us,
                app.arrival_span_us,app.pcm_queue_early,app.pcm_remaining,app.dac_irqs);
            printk("RX_PLAYBACK run=%u pcm_slab_min=%u pcm_slab_max=%u pcm_slab_last=%u slot_bits=32 pcm_bits=24 prefill_frames=3\n",
                run,app.pcm_queue_min,app.pcm_queue_max,app.pcm_queue_last);
            printk("RX_RESULT run=%u expected=%u good=%u missing=%u bad=%u packets=%u overflow=%u ipc_send_errors=%u app_wait=%d app_decoded=%u app_plc=%u app_decode_errors=%u app_ipc_drops=%u app_queue_peak=%u app_decode_avg_us=%u app_decode_max_us=%u app_playback_late=%u app_i2s_blocks=%u app_i2s_errors=%u app_i2s_underruns=%u app_i2s_last_error=%d app_volume_updates=%u app_volume_last=%u app_accel_irqs=%u app_accel_work=%u app_i2c_attempts=%u app_i2c_failures=%u app_flags=%u elapsed_us=%llu\n",
                run,expected,good,expected-good,bad,packets,rx_overflow,ipc_send_errors,
                app_wait,app.decoded,app.plc_frames,app.decode_errors,app.ipc_drops,
                app.queue_peak,app.decode_avg_us,app.decode_max_us,app.playback_late,
                app.i2s_blocks,app.i2s_errors,app.i2s_underruns,app.i2s_last_error,app.volume_updates,
                app.volume_last,app.accel_irqs,app.accel_work,app.i2c_attempts,
                app.i2c_failures,app.status_flags,now_us()-start);
            active=false;
        } else if(h.type==DATA && active && h.run==run) {
            packets++;
            unsigned n=p.length-sizeof(h);
            if(h.bytes>MAX_FRAME||h.offset+n>h.bytes||h.seq>=expected) { bad++; continue; }
            if(h.seq!=seq) { seq=h.seq; have=0; total=h.bytes; crc=h.crc; }
            if(h.offset!=have||h.bytes!=total||h.crc!=crc) { bad++; continue; }
            memcpy(frame+have,p.data+sizeof(h),n); have+=n;
            if(have==total) {
                if(crc32_ieee(frame,total)==crc) {
                    good++;
                    ipc_send_errors += rx_ipc_send_frame(run, h.seq, frame, total, crc) != 0;
                } else bad++;
            }
        } else if(h.type==VOLUME && active && h.run==run) {
            ipc_send_errors += rx_ipc_send_volume(run, h.seq, h.bytes) != 0;
        }
        if(now_us()-last>5000000) { last=now_us(); printk("RX_PROGRESS run=%u good=%u packets=%u\n",run,good,packets); }
    }
}
#endif
