/* Test receiver built on Zephyr USB Audio Class 2 (see SDK UAC2 samples).
 * The USB host is the source clock master; app_core recovers playback rate.
 * All assembly state is owned by the USB thread; main consumes copied frames. */
#include "usb_pcm.h"
#include <string.h>
#include <cmsis_core.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_uac2.h>

/* Zephyr sample VID/PID, local laboratory firmware only. */
USBD_DEVICE_DEFINE(bench_usbd,DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),0x2fe3,0x000e);
USBD_DESC_LANG_DEFINE(bench_lang);
USBD_DESC_MANUFACTURER_DEFINE(bench_mfr,"Audio research");
USBD_DESC_PRODUCT_DEFINE(bench_product,"nRF52840 PCM Bench");
USBD_DESC_SERIAL_NUMBER_DEFINE(bench_sn);
USBD_DESC_CONFIG_DEFINE(bench_cfg_desc,"PCM timing bench");
USBD_CONFIGURATION_DEFINE(bench_cfg,0,100,&bench_cfg_desc);
K_MEM_SLAB_DEFINE_STATIC(usb_buffers,304,8,4);
K_MEM_SLAB_DEFINE_STATIC(frame_pool,sizeof(struct usb_pcm_frame),18,8);
K_MSGQ_DEFINE(pcm_frames,sizeof(struct usb_pcm_frame *),16,4);
K_SEM_DEFINE(run_ready,0,1);

static const uint8_t marker[48]="NRFPCM24nrf52840NRFPCM24nrf52840NRFPCM24nrf52840";
static struct usb_pcm_run run;
static struct usb_pcm_stats stats;
static struct usb_pcm_frame *partial;
static unsigned marker_match, header_have, partial_have, produced;
static uint8_t command[24];
static bool reading_header, active;
static volatile bool run_owned;
static uint32_t le24(const uint8_t *p) { return p[0]|(p[1]<<8)|(p[2]<<16); }

static void inspect_byte(uint8_t b)
{
    if(reading_header) {
        command[header_have++]=b;
        if(header_have!=sizeof(command)) return;
        reading_header=false; header_have=0;
        uint32_t words[8];
        for(unsigned i=0;i<8;i++) words[i]=le24(command+3*i);
        bool valid=true;
        for(unsigned i=0;i<4;i++) valid &= (words[i]^words[i+4])==0xffffff;
        valid &= words[3]==0x544553 && words[0]>0 && words[2]>=100 && words[2]<=100000;
        valid &= words[1]>=320000 && words[1]<=400000 && !(words[1]%1000);
        if(valid && !run_owned) {
            run=(struct usb_pcm_run){words[0],words[1],words[2]};
            memset(&stats,0,sizeof(stats));
            produced=partial_have=0;
            run_owned=true; active=true;
            k_sem_give(&run_ready);
        }
    } else {
        marker_match = b==marker[marker_match] ? marker_match+1 : b==marker[0];
        if(marker_match==sizeof(marker)) { marker_match=0; reading_header=true; }
    }
}

static void terminal_cb(const struct device *dev,uint8_t terminal,bool enabled,bool micro,void *user)
{
    ARG_UNUSED(dev); ARG_UNUSED(terminal); ARG_UNUSED(micro); ARG_UNUSED(user);
    if(!enabled) {
        if(active) stats.disconnects++;
        active=false; marker_match=header_have=partial_have=0; reading_header=false;
        if(partial) { k_mem_slab_free(&frame_pool,partial); partial=NULL; }
    }
    printk("USB_TERMINAL enabled=%u\n",enabled);
}
static void *get_buf(const struct device *dev,uint8_t terminal,uint16_t size,void *user)
{
    ARG_UNUSED(dev); ARG_UNUSED(terminal); ARG_UNUSED(user);
    void *p=NULL;
    if(size>304 || k_mem_slab_alloc(&usb_buffers,&p,K_NO_WAIT)) {
        if(active) stats.buffer_fail++;
        return NULL;
    }
    return p;
}
static void received(const struct device *dev,uint8_t terminal,void *buf,uint16_t size,void *user)
{
    ARG_UNUSED(dev); ARG_UNUSED(terminal); ARG_UNUSED(user);
    uint32_t c0=DWT->CYCCNT;
    bool measured=active;
    if(active) {
        stats.packets++;
        stats.zero_packets+=size==0;
        stats.malformed+=size%6!=0;
    }
    uint64_t packet_ready=now_us();
    const uint8_t *p=buf;
    for(unsigned i=0;i<size;) {
        if(!active) { inspect_byte(p[i++]); continue; }
        if(!partial_have && !partial) {
            (void)k_mem_slab_alloc(&frame_pool,(void **)&partial,K_NO_WAIT);
        }
        unsigned count=MIN(size-i,USB_PCM_BYTES-partial_have);
        if(partial) memcpy(partial->data+partial_have,p+i,count);
        partial_have+=count; i+=count;
        if(partial_have==USB_PCM_BYTES) {
            if(partial) { partial->ready_us=packet_ready; partial->sequence=produced; }
            produced++;
            partial_have=0;
            if(!partial || k_msgq_put(&pcm_frames,&partial,K_NO_WAIT)) {
                stats.overflows++;
                if(partial) k_mem_slab_free(&frame_pool,partial);
            }
            else {
                stats.queued_frames++;
                stats.queue_peak=MAX(stats.queue_peak,k_msgq_num_used_get(&pcm_frames));
            }
            partial=NULL; /* queue/consumer owns completed buffers */
            if(produced==run.frames+USB_WARMUP_FRAMES) active=false;
        }
    }
    if(buf) k_mem_slab_free(&usb_buffers,buf);
    if(measured) {
        uint32_t cycles=DWT->CYCCNT-c0;
        stats.callbacks++; stats.callback_cycles+=cycles;
        stats.callback_max_cycles=MAX(stats.callback_max_cycles,cycles);
    }
}
static void sof(const struct device *dev,void *user) { ARG_UNUSED(dev); ARG_UNUSED(user); }
static struct uac2_ops ops={.sof_cb=sof,.terminal_update_cb=terminal_cb,
    .get_recv_buf=get_buf,.data_recv_cb=received};

int usb_pcm_init(void)
{
    usbd_uac2_set_ops(DEVICE_DT_GET(DT_NODELABEL(bench_uac2)),&ops,NULL);
    int rc=usbd_add_descriptor(&bench_usbd,&bench_lang);
    if(!rc) rc=usbd_add_descriptor(&bench_usbd,&bench_mfr);
    if(!rc) rc=usbd_add_descriptor(&bench_usbd,&bench_product);
    if(!rc) rc=usbd_add_descriptor(&bench_usbd,&bench_sn);
    if(!rc) rc=usbd_add_configuration(&bench_usbd,USBD_SPEED_FS,&bench_cfg);
    const char *const blocked[]={NULL};
    if(!rc) rc=usbd_register_all_classes(&bench_usbd,USBD_SPEED_FS,1,blocked);
    usbd_device_set_code_triple(&bench_usbd,USBD_SPEED_FS,USB_BCC_MISCELLANEOUS,2,1);
    if(!rc) rc=usbd_init(&bench_usbd);
    if(!rc) rc=usbd_enable(&bench_usbd);
    return rc;
}
int usb_pcm_wait_run(struct usb_pcm_run *out)
{
    k_sem_take(&run_ready,K_FOREVER); *out=run; return 0;
}
int usb_pcm_get(struct usb_pcm_frame **frame) { return k_msgq_get(&pcm_frames,frame,K_SECONDS(2)); }
void usb_pcm_release(struct usb_pcm_frame *frame) { k_mem_slab_free(&frame_pool,frame); }
void usb_pcm_finish(struct usb_pcm_stats *out)
{
    /* USB threads have cooperative priority above main. A short interrupt lock
     * prevents a callback observing partially reset ownership or statistics. */
    unsigned key=irq_lock();
    active=false; *out=stats;
    struct usb_pcm_frame *old;
    while(!k_msgq_get(&pcm_frames,&old,K_NO_WAIT)) k_mem_slab_free(&frame_pool,old);
    if(partial) { k_mem_slab_free(&frame_pool,partial); partial=NULL; }
    run_owned=false;
    irq_unlock(key);
}
