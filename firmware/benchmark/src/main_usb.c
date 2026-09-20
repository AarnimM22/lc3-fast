#include "usb_pcm.h"
#include "lc3_custom.h"
#include <string.h>
#include <cmsis_core.h>
#include <zephyr/sys/crc.h>

static struct usb_pcm_frame *input;
static uint8_t encoded[204];
static uint64_t encoded_hash;
/* Pair the RTC-based wall time and cycle counter under a short IRQ lock.
 * Otherwise an IRQ between the two reads can look like a timer discrepancy. */
static uint64_t timing_mark(uint32_t *cycles)
{
    unsigned key=irq_lock();
    uint64_t wall=now_us();
    *cycles=DWT->CYCCNT;
    irq_unlock(key);
    return wall;
}
static void fingerprint_packet(uint32_t crc,unsigned bytes)
{
    encoded_hash=(encoded_hash^((uint64_t)crc|((uint64_t)bytes<<32)))*1099511628211ull;
}
static uint32_t us(uint64_t cycles) { return cycles*1000000u/SystemCoreClock; }
/* Test-only whole-stream integrity fingerprint. Word-wise FNV-1a with the
 * standard 64-bit offset/prime, explicitly little endian (not byte-wise FNV).
 * Avoid charging a byte-wise software CRC pass to the production PCM path. */
#if !OPT_LEAN
static uint64_t pcm_hash(uint64_t hash,const uint8_t *p,unsigned n)
{
    for(unsigned i=0;i<n;i+=4) {
        uint32_t word; memcpy(&word,p+i,4);
        hash=(hash^word)*1099511628211ull;
    }
    return hash;
}
#endif
static int control(unsigned type,unsigned id,unsigned frames)
{
    struct header h={.magic=MAGIC,.run=id,.seq=frames,.type=type};
    for(unsigned i=0;i<3;i++) if(!radio_send(&h,sizeof(h))) return 0;
    return -EIO;
}
#if !OPT_TRANSPORT
static int submit(unsigned id,unsigned seq,unsigned bytes,uint64_t ready)
{
    uint8_t p[252];
    struct header h={.magic=MAGIC,.run=id,.seq=seq,.crc=crc32_ieee(encoded,bytes),
        .bytes=bytes,.type=DATA};
    fingerprint_packet(h.crc,bytes);
    memcpy(p,&h,sizeof(h)); memcpy(p+sizeof(h),encoded,bytes);
    return radio_async_submit(p,sizeof(h)+bytes,ready,ready+5000);
}
#endif
/* Inject a small, acknowledged control packet while audio is flowing.  The
 * command uses the same bounded radio queue as audio and never waits for an
 * I2C transaction on the receiver. */
static int submit_volume(unsigned id,unsigned seq,uint16_t level,uint64_t ready)
{
    struct header h={.magic=MAGIC,.run=id,.seq=seq,.bytes=level,.type=VOLUME};
#if OPT_TRANSPORT
    struct radio_owned_packet *packet=radio_async_acquire();
    if (!packet) return -ENOMEM;
    memcpy(packet->data,&h,sizeof(h));
    packet->length=sizeof(h); packet->ready_us=ready; packet->deadline_us=ready+5000;
    return radio_async_commit(packet);
#else
    return radio_async_submit(&h,sizeof(h),ready,ready+5000);
#endif
}
static void run_case(const struct usb_pcm_run *r)
{
    lc3_custom_configure(CUSTOM_SNS_COARSE,CUSTOM_VBR_ANALYTIC,0);
    int rc=codec_init(2500,r->bitrate);
    if(rc) { printk("USB_CONFIG_ERROR rc=%d\n",rc); return; }
    unsigned sequence_errors=0,codec_errors=0,send_errors=0,timeout=0;
    unsigned producer_late=0,work_over_period=0,timing_errors=0,completed=0;
    unsigned max_work=0,max_api=0,max_age=0,max_queue_age=0,min_bytes=204,max_bytes=0;
    uint64_t api_cycles=0,work_total=0,payload=0,first_ready=0,last_ready=0;
    uint64_t prep_cycles=0,submit_cycles=0;
    uint32_t timing_details[4][3]={0};
    uint64_t processing_start=0;
    uint64_t input_hash=14695981039346656037ull;
    uint32_t ret0=tx_retries,fail0=tx_fail;
    for(unsigned i=0;i<USB_WARMUP_FRAMES;i++) {
        if(usb_pcm_get(&input)) { timeout++; goto done; }
        sequence_errors+=input->sequence!=i;
        unsigned bytes=0;
        codec_errors+=codec_encode_usb(i,input->data,encoded,&bytes)!=0;
        usb_pcm_release(input);
    }
    if(radio_async_reset()) { send_errors++; goto done; }
    send_errors+=control(START,r->id,r->frames)!=0;
    lc3_lite_set_flags(LITE_NO_LTPF|LITE_NO_TNS); /* Reset stage counters only. */
    custom_cap_frames=custom_sns_clips=0;
    encoded_hash=14695981039346656037ull;
    ret0=tx_retries; fail0=tx_fail;
    printk("USB_BEGIN id=%u bitrate=%u frames=%u warmup=%u pcm_bits=24 frame_us=2500 optimizations=%u pcm_hash_enabled=%u profile_enabled=%u\n",
        r->id,r->bitrate,r->frames,USB_WARMUP_FRAMES,BENCH_OPTIMIZATIONS,!OPT_LEAN,!OPT_LEAN);
    processing_start=now_us();
    for(unsigned seq=0;seq<r->frames;seq++) {
        if(usb_pcm_get(&input)) { timeout++; break; }
        uint32_t c0;
        uint64_t start=timing_mark(&c0);
        if(!seq) first_ready=input->ready_us;
        last_ready=input->ready_us;
        max_queue_age=MAX(max_queue_age,start-input->ready_us);
        sequence_errors+=input->sequence!=USB_WARMUP_FRAMES+seq;
        /* Includes whole-stream PCM integrity checking and enqueue. The codec
         * itself loads packed samples, avoiding a separate unpack/copy pass. */
#if !OPT_LEAN
        input_hash=pcm_hash(input_hash,input->data,USB_PCM_BYTES);
#endif
        uint8_t *output=encoded;
#if OPT_TRANSPORT
        struct radio_owned_packet *packet=radio_async_acquire();
        if (!packet) { send_errors++; usb_pcm_release(input); break; }
        output=packet->data+sizeof(struct header);
#endif
        unsigned bytes=0;
        uint32_t prepared=DWT->CYCCNT;
        prep_cycles+=prepared-c0;
        codec_errors+=codec_encode_usb(USB_WARMUP_FRAMES+seq,input->data,output,&bytes)!=0;
        api_cycles+=codec_api_cycles; max_api=MAX(max_api,codec_api_cycles);
        if(bytes>204 || !bytes) {
            codec_errors++; usb_pcm_release(input);
#if OPT_TRANSPORT
            radio_async_cancel(packet);
#endif
            break;
        }
        uint32_t submit_start=DWT->CYCCNT;
        uint64_t ready=input->ready_us;
#if OPT_TRANSPORT
        struct header h={.magic=MAGIC,.run=r->id,.seq=seq,
            .crc=bench_crc32(output,bytes),.bytes=bytes,.type=DATA};
        fingerprint_packet(h.crc,bytes);
        memcpy(packet->data,&h,sizeof(h));
        packet->length=sizeof(h)+bytes; packet->ready_us=ready;
        packet->deadline_us=ready+5000;
        send_errors+=radio_async_commit(packet)!=0;
#else
        send_errors+=submit(r->id,seq,bytes,ready)!=0;
#endif
        /* Four volume changes per second are enough to exercise the wireless
         * control path and the receiver's DAC-I2C work while the codec and
         * radio remain at the 384 kb/s audio cadence. */
        if ((seq % 100u) == 50u) {
            static const uint16_t levels[] = {24576, 32768, 40960, 28672};
            unsigned command=(seq / 100u) % ARRAY_SIZE(levels);
#if OPT_TRANSPORT
            send_errors+=submit_volume(r->id,seq,levels[command],ready)!=0;
#else
            send_errors+=submit_volume(r->id,seq,levels[command],ready)!=0;
#endif
        }
        usb_pcm_release(input);
        submit_cycles+=DWT->CYCCNT-submit_start;
        uint32_t c1;
        uint64_t finish=timing_mark(&c1);
        uint32_t dc=c1-c0;
        unsigned work=finish-start, age=finish-ready;
        if(!dc || us(dc)+200<work || work+200<us(dc)) {
            if(timing_errors<4) {
                timing_details[timing_errors][0]=seq;
                timing_details[timing_errors][1]=us(dc);
                timing_details[timing_errors][2]=work;
            }
            timing_errors++;
        }
        work_total+=work; max_work=MAX(max_work,work);
        max_age=MAX(max_age,age); producer_late+=age>2500;
        work_over_period+=work>2500;
        payload+=bytes; min_bytes=MIN(min_bytes,bytes); max_bytes=MAX(max_bytes,bytes);
        completed++;
    }
done: ;
    struct radio_async_stats radio={0};
    send_errors+=radio_async_drain(&radio)!=0;
    uint64_t end=now_us();
    struct usb_pcm_stats usb;
    usb_pcm_finish(&usb);
    if(processing_start) send_errors+=control(END,r->id,r->frames)!=0;
    printk("USB_RESULT id=%u bitrate=%u expected=%u frames=%u pcm_hash=%016llx api_avg_us=%u api_max_us=%u work_avg_us=%u work_max_us=%u work_over_period=%u producer_late=%u ready_to_submit_max_us=%u queue_age_max_us=%u sequence_errors=%u codec_errors=%u send_errors=%u timeout=%u timing_errors=%u payload_bytes=%llu payload_min=%u payload_max=%u radio_completed=%u radio_late=%u ready_to_ack_avg_us=%u ready_to_ack_max_us=%u pending_peak=%u retries=%u ack_fail=%u elapsed_us=%llu input_span_us=%llu cap_channels=%u sns_clips=%u\n",
        r->id,r->bitrate,r->frames,completed,input_hash,us(api_cycles/MAX(completed,1)),us(max_api),
        (unsigned)(work_total/MAX(completed,1)),max_work,work_over_period,producer_late,max_age,max_queue_age,
        sequence_errors,codec_errors,send_errors+radio.errors,timeout,timing_errors,payload,min_bytes,max_bytes,
        radio.completed,radio.late,(unsigned)(radio.latency_total_us/MAX(radio.completed,1)),radio.latency_max_us,
        radio.peak_pending,tx_retries-ret0,tx_fail-fail0,processing_start ? end-processing_start : 0,
        last_ready-first_ready,custom_cap_frames,custom_sns_clips);
    printk("USB_INPUT id=%u packets=%u zero_packets=%u malformed=%u buffer_fail=%u overflows=%u queued_frames=%u queue_peak=%u disconnects=%u callbacks=%u callback_avg_us=%u callback_max_us=%u callback_total_us=%u\n",
        r->id,usb.packets,usb.zero_packets,usb.malformed,usb.buffer_fail,usb.overflows,
        usb.queued_frames,usb.queue_peak,usb.disconnects,usb.callbacks,
        us(usb.callback_cycles/MAX(usb.callbacks,1)),us(usb.callback_max_cycles),us(usb.callback_cycles));
    printk("USB_PROFILE id=%u",r->id);
    const char *names[]={"attack","ltpf","mdct","energy","bw","sns","tns","spec","bits","gain"};
    for(unsigned i=0;i<PROF_COUNT;i++) printk(" %s_us=%u",names[i],us(lc3_lite_cycles[i]/MAX(completed,1)));
    printk("\n");
    printk("USB_OVERHEAD id=%u prep_avg_us=%u submit_avg_us=%u\n",r->id,
        us(prep_cycles/MAX(completed,1)),us(submit_cycles/MAX(completed,1)));
    printk("USB_PAYLOAD id=%u packet_hash=%016llx optimizations=%u pcm_hash_enabled=%u profile_enabled=%u\n",
        r->id,encoded_hash,BENCH_OPTIMIZATIONS,!OPT_LEAN,!OPT_LEAN);
    for(unsigned i=0;i<MIN(timing_errors,4);i++)
        printk("USB_TIMING id=%u seq=%u cycles_us=%u wall_us=%u\n",r->id,
            timing_details[i][0],timing_details[i][1],timing_details[i][2]);
    printk("USB_DONE id=%u\n",r->id);
}
int main(void)
{
    CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;
    int radio=radio_init(),usb=usb_pcm_init();
    printk("USB_BOOT radio=%d usb=%d cpu_hz=%u codec=%s\n",radio,usb,SystemCoreClock,codec_name());
    if(radio || usb) return 0;
    for(;;) {
        printk("USB_WAIT marker=NRFPCM24 sample_rate=48000 channels=2 bits=24\n");
        struct usb_pcm_run run;
        usb_pcm_wait_run(&run); run_case(&run);
    }
}
