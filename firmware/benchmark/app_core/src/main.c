#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <cmsis_core.h>
#include <hal/nrf_clock.h>

#if defined(CONFIG_SOC_NRF5340_CPUAPP)
#include <nrf53_cpunet_mgmt.h>
#endif

#include "lc3_custom.h"
#include "rx_ipc.h"

#define APP_FRAME_SAMPLES 120
#define APP_CHANNELS 2
#define APP_FRAME_PERIOD_US 2500
#define APP_I2S_BLOCK_BYTES (APP_FRAME_SAMPLES * APP_CHANNELS * sizeof(int32_t))
#define APP_I2S_PREFILL 3

enum {
    APP_STATUS_IPC = 1u << 0,
    APP_STATUS_I2S = 1u << 1,
    APP_STATUS_I2C = 1u << 2,
    APP_STATUS_DECODER = 1u << 3,
};

static const struct device *ipc_instance;
static struct ipc_ept endpoint;
static K_SEM_DEFINE(bound_sem, 0, 1);
static volatile bool endpoint_bound;

static struct rx_ipc_message message_q_buf;
K_MSGQ_DEFINE(message_q, sizeof(struct rx_ipc_message), 8, 4);
static volatile uint32_t ipc_drops;

static const struct device *i2s_dev;
K_MEM_SLAB_DEFINE_STATIC(i2s_slab, APP_I2S_BLOCK_BYTES, 8, 4);
static bool i2s_configured;
static bool i2s_started;
static uint32_t i2s_blocks;
static uint32_t i2s_errors;
static uint32_t i2s_underruns;
static int32_t i2s_last_error;
static volatile uint32_t dma_released;
static volatile uint64_t dma_first_us, dma_last_us;
static volatile uint32_t dma_gap_max_us;
static uint32_t arrival_count;
static uint64_t arrival_first_us, arrival_last_us;
static uint64_t now_us(void);

/* Slow audio PLL servo. Observe source sample position against DMA position,
 * including fractional progress since the last completion. One-second means
 * reject codec/radio/USB packet jitter; five means establish the startup lead.
 * All observation state is protected by irq_lock across IPC/main contexts.
 * The rate actuator changes clocks only, never the PCM sample sequence. */
static int64_t clock_lead_sum;
static uint32_t clock_observations, clock_prev_seq;
static uint64_t clock_prev_arrival;
static int64_t clock_target_sum;
static uint32_t clock_calibration, clock_updates, clock_rejected;
static int32_t clock_target_us, clock_error_us, clock_peak_error_us;
static int32_t clock_integral_mppm, clock_steps;
static uint16_t clock_nominal;
static int32_t clock_test_bias;

static void clock_observe(uint32_t seq)
{
    unsigned key = irq_lock();
    /* Time and DMA position must belong to the same atomic snapshot. */
    uint64_t t = now_us();
    bool continuous = seq == clock_prev_seq + 1 &&
                      t - clock_prev_arrival < 100000;
    clock_prev_seq = seq;
    clock_prev_arrival = t;
    if (seq >= 400 && continuous && dma_released &&
        t >= dma_last_us && t - dma_last_us < 5000) {
        int64_t lead = ((int64_t)seq + 1 - dma_released) * 2500 -
                       (int64_t)(t - dma_last_us);
        if (lead >= 0 && lead <= 20000) {
            clock_lead_sum += lead;
            clock_observations++;
        } else clock_rejected++;
    } else if (seq >= 400) {
        clock_rejected++;
        clock_lead_sum = 0;
        clock_observations = 0;
    }
    irq_unlock(key);
}

static void clock_update(void)
{
    unsigned key = irq_lock();
    if (clock_observations < 400) { irq_unlock(key); return; }
    int64_t sum = clock_lead_sum;
    uint32_t count = clock_observations;
    clock_lead_sum = 0;
    clock_observations = 0;
    irq_unlock(key);
    int32_t lead = sum / count;
    if (clock_calibration < 5) {
        clock_target_sum += lead;
        if (++clock_calibration == 5) clock_target_us = clock_target_sum / 5;
        return;
    }
    clock_error_us = lead - clock_target_us;
    clock_peak_error_us = MAX(clock_peak_error_us, abs(clock_error_us));
    /* P: 1/10 s; I: 1/400 s^2. 50 us deadband exceeds the 30.5 us
     * RTC resolution. Integral retains learned frequency inside deadband.
     * Clamp and slew limiting bound windup and audio frequency modulation. */
    int32_t error = clock_error_us > 50 ? clock_error_us - 50 :
                    clock_error_us < -50 ? clock_error_us + 50 : 0;
    clock_integral_mppm = CLAMP(clock_integral_mppm +
        (int32_t)((int64_t)error * count / 160), -150000, 150000);
    int32_t mppm = CLAMP(clock_integral_mppm + error * 100, -200000, 200000);
    /* Fout=(32 MHz/12)*(4+FREQUENCY/65536): one step ~3.31137 ppm. */
    int32_t wanted = (mppm + (mppm >= 0 ? 1656 : -1656)) / 3311;
    clock_steps += CLAMP(wanted - clock_steps, -4, 4);
    clock_updates++;
#if AUDIO_CLOCK_STRESS
    /* Deliberate actuator disturbances (~+99 and -99 ppm). The estimator
     * has no feed-forward knowledge of these offsets and must recover. */
    if (clock_updates == 44) clock_test_bias = 30;
    if (clock_updates == 119) clock_test_bias = -30;
#endif
    nrf_clock_hfclkaudio_config_set(NRF_CLOCK,
                                  clock_nominal + clock_steps + clock_test_bias);
    if (clock_updates % 10 == 0)
        printk("APP_SERVO update=%u lead_us=%d target_us=%d error_us=%d steps=%d bias=%d integral_mppm=%d\n",
               clock_updates, lead, clock_target_us, clock_error_us,
               clock_steps, clock_test_bias, clock_integral_mppm);
}

void __real_k_mem_slab_free(struct k_mem_slab *slab, void *block);
void __wrap_k_mem_slab_free(struct k_mem_slab *slab, void *block)
{
    /* With no I2S errors these IRQ releases represent completed PCM blocks.
     * Thread-side frees of rejected writes are deliberately excluded. */
    if (slab == &i2s_slab && k_is_in_isr()) {
        uint64_t t = now_us();
        if (!dma_released) dma_first_us = t;
        else dma_gap_max_us = MAX(dma_gap_max_us, (uint32_t)(t - dma_last_us));
        dma_last_us = t;
        dma_released++;
    }
    __real_k_mem_slab_free(slab, block);
}

static const struct device *i2c_dev;
static volatile uint16_t pending_volume = 32768;
static uint32_t i2c_attempts;
static uint32_t i2c_failures;

static uint32_t accel_irqs;
static uint32_t accel_work_count;
static uint32_t accel_filter_state;
static void accel_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    uint32_t x = accel_filter_state + accel_irqs * 17u;
    for (unsigned i = 0; i < 48; i++) {
        x = (x * 1664525u) + 1013904223u;
        x ^= x >> 13;
    }
    accel_filter_state = x;
    accel_work_count++;
}
K_WORK_DEFINE(accel_work, accel_work_handler);

static void accel_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    accel_irqs++;
    (void)k_work_submit(&accel_work);
}
K_TIMER_DEFINE(accel_timer, accel_timer_handler, NULL);

static uint32_t dac_irqs;
static void dac_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    dac_irqs++;
    /* Synthetic 100 Hz DAC status IRQ; deliberately spend 20 us in ISR. */
    k_busy_wait(20);
}
K_TIMER_DEFINE(dac_timer, dac_timer_handler, NULL);

static void i2c_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!i2c_dev || !device_is_ready(i2c_dev)) {
        i2c_failures++;
        return;
    }

    uint16_t volume = pending_volume;
    /* A short register transaction is representative of CS43131 volume and
     * mode updates.  The DK has no codec fitted, so a NACK is expected. */
    uint8_t command[4] = {0x04, (uint8_t)(volume >> 8), (uint8_t)volume, 0};
    i2c_attempts++;
    if (i2c_write(i2c_dev, command, sizeof(command), 0x30) < 0) {
        i2c_failures++;
    }
}
K_WORK_DEFINE(i2c_work, i2c_work_handler);

static uint64_t now_us(void)
{
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

static void endpoint_bound_cb(void *priv)
{
    ARG_UNUSED(priv);
    endpoint_bound = true;
    k_sem_give(&bound_sem);
}

static void endpoint_unbound_cb(void *priv)
{
    ARG_UNUSED(priv);
    endpoint_bound = false;
}

static void endpoint_received_cb(const void *data, size_t len, void *priv)
{
    ARG_UNUSED(priv);
    if (len < offsetof(struct rx_ipc_message, data) ||
        len > sizeof(struct rx_ipc_message)) {
        ipc_drops++;
        return;
    }

    struct rx_ipc_message message;
    memset(&message, 0, sizeof(message));
    memcpy(&message, data, len);
    if (message.magic != RX_IPC_MAGIC ||
        (message.type == RX_IPC_FRAME &&
         (message.bytes > RX_IPC_MAX_FRAME ||
          len != offsetof(struct rx_ipc_message, data) + message.bytes))) {
        ipc_drops++;
        return;
    }
    if (message.type == RX_IPC_START) {
        arrival_count = 0;
        arrival_first_us = arrival_last_us = 0;
    } else if (message.type == RX_IPC_FRAME) {
        uint64_t t = now_us();
        if (!arrival_count) arrival_first_us = t;
        arrival_last_us = t;
        arrival_count++;
        clock_observe(message.seq);
    }
    if (k_msgq_put(&message_q, &message, K_NO_WAIT)) {
        ipc_drops++;
    }
}

static void endpoint_error_cb(const char *message, void *priv)
{
    ARG_UNUSED(priv);
    printk("APP_IPC_ERROR %s\n", message ? message : "unknown");
}

static const struct ipc_ept_cfg endpoint_cfg = {
    .name = "audio",
    .cb = {
        .bound = endpoint_bound_cb,
        .unbound = endpoint_unbound_cb,
        .received = endpoint_received_cb,
        .error = endpoint_error_cb,
    },
};

static int ipc_init(void)
{
#if defined(CONFIG_SOC_NRF5340_CPUAPP)
    (void)nrf53_cpunet_enable(true);
#endif
    ipc_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));
    if (!device_is_ready(ipc_instance)) return -ENODEV;

    int rc = ipc_service_open_instance(ipc_instance);
    if (rc < 0 && rc != -EALREADY) return rc;
    rc = ipc_service_register_endpoint(ipc_instance, &endpoint, &endpoint_cfg);
    if (rc < 0) return rc;
    rc = k_sem_take(&bound_sem, K_MSEC(3000));
    if (rc) return rc;
    return 0;
}

static int ipc_send_status(const struct rx_app_status *status)
{
    if (!endpoint_bound) return -ENOTCONN;
    for (unsigned i = 0; i < 200; i++) {
        int rc = ipc_service_send(&endpoint, status, sizeof(*status));
        if (rc >= 0) return 0;
        if (rc != -ENOMEM) return rc;
        k_busy_wait(25);
    }
    return -EAGAIN;
}

static int i2s_init(void)
{
    i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
    if (!device_is_ready(i2s_dev)) return -ENODEV;

    struct i2s_config config = {
        .word_size = 32, /* S24, left-aligned with eight zero padding bits. */
        .channels = 2,
        .format = I2S_FMT_DATA_FORMAT_I2S,
        .options = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER,
        .frame_clk_freq = 48000,
        .block_size = APP_I2S_BLOCK_BYTES,
        .timeout = 1,
        .mem_slab = &i2s_slab,
    };
    int rc = i2s_configure(i2s_dev, I2S_DIR_TX, &config);
    if (rc) return rc;
    i2s_configured = true;
    return 0;
}

static int i2s_write_block(const int32_t *samples)
{
    if (!i2s_configured) return -ENODEV;
    void *block = NULL;
    int rc = k_mem_slab_alloc(&i2s_slab, &block, K_NO_WAIT);
    if (rc) {
        i2s_errors++;
        i2s_last_error = rc;
        return rc;
    }
    memcpy(block, samples, APP_I2S_BLOCK_BYTES);
    rc = i2s_write(i2s_dev, block, APP_I2S_BLOCK_BYTES);
    if (rc) {
        k_mem_slab_free(&i2s_slab, block);
        i2s_errors++;
        i2s_last_error = rc;
        return rc;
    }
    i2s_blocks++;
    return 0;
}

static void i2s_prefill_and_start(void)
{
    /* Called only after three decoded PCM blocks have been queued. */
    int rc = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    printk("APP_I2S_START rc=%d blocks=%u\n", rc, i2s_blocks);
    if (rc) {
        i2s_errors++;
        i2s_last_error = rc;
        return;
    }
    i2s_started = true;
}

static void i2s_stop(void)
{
    if (!i2s_started) return;
    int rc = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    if (rc) {
        /* ERROR has already uninitialized nrfx; DROP would stop it twice. */
        (void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
        i2s_errors++;
        i2s_last_error = rc;
    }
    i2s_started = false;
}

static void schedule_i2c(void)
{
    (void)k_work_submit(&i2c_work);
}

static int decode_frame(lc3_decoder_t *decoders, const uint8_t *data,
                        unsigned bytes, int32_t *interleaved)
{
    int32_t pcm[APP_CHANNELS][APP_FRAME_SAMPLES];
    unsigned offset = 0;
    for (unsigned channel = 0; channel < APP_CHANNELS; channel++) {
        if (offset + 2 > bytes) return -EBADMSG;
        unsigned payload = data[offset + 1];
        unsigned packet = payload + 2;
        if (payload < 20 || offset + packet > bytes) return -EBADMSG;
        int rc = lc3_custom_decode(decoders[channel], data + offset, packet,
                                   LC3_PCM_FORMAT_S24, pcm[channel], 1);
        if (rc < 0) return rc;
        offset += packet;
    }
    if (offset != bytes) return -EBADMSG;

    for (unsigned i = 0; i < APP_FRAME_SAMPLES; i++) {
        for (unsigned channel = 0; channel < APP_CHANNELS; channel++) {
            int64_t scaled = ((int64_t)pcm[channel][i] * pending_volume) >> 15;
            if (scaled > 8388607) scaled = 8388607;
            if (scaled < -8388608) scaled = -8388608;
            interleaved[i * APP_CHANNELS + channel] = (int32_t)(scaled * 256);
        }
    }
    return 0;
}

static int decode_plc(lc3_decoder_t *decoders, int32_t *interleaved)
{
    int32_t pcm[APP_CHANNELS][APP_FRAME_SAMPLES];
    for (unsigned channel = 0; channel < APP_CHANNELS; channel++) {
        int rc = lc3_custom_decode(decoders[channel], NULL, 0,
                                   LC3_PCM_FORMAT_S24, pcm[channel], 1);
        if (rc < 0) return rc;
    }
    for (unsigned i = 0; i < APP_FRAME_SAMPLES; i++) {
        for (unsigned channel = 0; channel < APP_CHANNELS; channel++) {
            int64_t scaled = ((int64_t)pcm[channel][i] * pending_volume) >> 15;
            if (scaled > 8388607) scaled = 8388607;
            if (scaled < -8388608) scaled = -8388608;
            interleaved[i * APP_CHANNELS + channel] = (int32_t)(scaled * 256);
        }
    }
    return 0;
}

static struct rx_app_status app_status;
static lc3_decoder_mem_48k_t decoder_memory[APP_CHANNELS];
static lc3_decoder_t decoders[APP_CHANNELS];
static int32_t i2s_samples[APP_FRAME_SAMPLES * APP_CHANNELS];
static uint64_t first_frame_us;
static uint32_t next_sequence;
static bool active;

static void reset_run(const struct rx_ipc_message *message)
{
    memset(&app_status, 0, sizeof(app_status));
    app_status.magic = RX_IPC_MAGIC;
    app_status.run = message->run;
    app_status.expected = message->seq;
    app_status.volume_last = pending_volume;
    first_frame_us = 0;
    next_sequence = 0;
    active = true;
    i2s_blocks = i2s_errors = i2s_underruns = 0;
    i2s_last_error = 0;
    dma_released = dma_gap_max_us = 0;
    dma_first_us = dma_last_us = 0;
    dac_irqs = 0;
    i2c_attempts = i2c_failures = 0;
    accel_irqs = accel_work_count = 0;
    ipc_drops = 0;
    i2s_started = false;
    app_status.pcm_queue_min = UINT32_MAX;
    unsigned key = irq_lock();
    clock_lead_sum = clock_target_sum = 0;
    clock_observations = clock_calibration = clock_updates = clock_rejected = 0;
    clock_prev_seq = UINT32_MAX;
    clock_prev_arrival = 0;
    clock_error_us = clock_peak_error_us = clock_target_us = 0;
    clock_integral_mppm = clock_steps = 0;
    clock_test_bias = 0;
    irq_unlock(key);
    nrf_clock_hfclkaudio_config_set(NRF_CLOCK, clock_nominal);
    k_timer_start(&accel_timer, K_MSEC(1), K_MSEC(1));
    k_timer_start(&dac_timer, K_MSEC(10), K_MSEC(10));
}

static void output_one_frame(uint32_t sequence, bool plc)
{
    uint64_t started = now_us();
    int rc = plc ? decode_plc(decoders, i2s_samples)
                 : decode_frame(decoders, message_q_buf.data,
                                message_q_buf.bytes, i2s_samples);
    uint32_t elapsed = (uint32_t)(now_us() - started);
    app_status.decode_avg_us += elapsed;
    app_status.decode_max_us = MAX(app_status.decode_max_us, elapsed);
    if (rc) {
        app_status.decode_errors++;
        memset(i2s_samples, 0, sizeof(i2s_samples));
    }
    if (!first_frame_us) first_frame_us = started;
    uint64_t deadline = first_frame_us + (uint64_t)(sequence + APP_I2S_PREFILL + 1) * APP_FRAME_PERIOD_US;
    if (now_us() > deadline) app_status.playback_late++;
    if (i2s_write_block(i2s_samples)) {
        i2s_underruns++;
    }
    if (!i2s_started && i2s_blocks == APP_I2S_PREFILL) i2s_prefill_and_start();
    if (i2s_started) {
        unsigned used = k_mem_slab_num_used_get(&i2s_slab);
        app_status.pcm_queue_min = MIN(app_status.pcm_queue_min, used);
        app_status.pcm_queue_max = MAX(app_status.pcm_queue_max, used);
        app_status.pcm_queue_last = used;
        if (sequence == 999) app_status.pcm_queue_early = used;
    }
    app_status.decoded++;
    if (plc) app_status.plc_frames++;
    next_sequence = sequence + 1;
    clock_update();
    if ((sequence & 15u) == 15u) schedule_i2c();
}

static void finish_run(void)
{
    while (next_sequence < app_status.expected) {
        output_one_frame(next_sequence, true);
    }
    k_timer_stop(&accel_timer);
    k_timer_stop(&dac_timer);
    i2s_stop();
    k_msleep(20); /* Allow the queued final audio to drain before reporting. */
    app_status.dma_released = dma_released;
    app_status.dma_span_us = dma_last_us - dma_first_us;
    app_status.dma_gap_max_us = dma_gap_max_us;
    app_status.arrival_span_us = arrival_last_us - arrival_first_us;
    app_status.dac_irqs = dac_irqs;
    app_status.pcm_remaining = k_mem_slab_num_used_get(&i2s_slab);
    app_status.clock_updates = clock_updates;
    app_status.clock_steps = clock_steps;
    app_status.clock_error_us = clock_error_us;
    app_status.clock_peak_error_us = clock_peak_error_us;
    app_status.clock_rejected = clock_rejected;
    app_status.clock_test_bias = clock_test_bias;
    app_status.queue_peak = MAX(app_status.queue_peak, k_msgq_num_used_get(&message_q));
    app_status.decode_avg_us = app_status.decoded ?
        app_status.decode_avg_us / app_status.decoded : 0;
    app_status.ipc_drops = ipc_drops;
    app_status.i2s_blocks = i2s_blocks;
    app_status.i2s_errors = i2s_errors;
    app_status.i2s_underruns = i2s_underruns;
    app_status.i2s_last_error = i2s_last_error;
    app_status.accel_irqs = accel_irqs;
    app_status.accel_work = accel_work_count;
    app_status.i2c_attempts = i2c_attempts;
    app_status.i2c_failures = i2c_failures;
    app_status.status_flags = APP_STATUS_IPC | APP_STATUS_DECODER;
    if (i2s_configured) app_status.status_flags |= APP_STATUS_I2S;
    if (i2c_dev && device_is_ready(i2c_dev)) app_status.status_flags |= APP_STATUS_I2C;
    (void)ipc_send_status(&app_status);
    active = false;
}

static void process_message(const struct rx_ipc_message *message)
{
    switch (message->type) {
    case RX_IPC_START:
        reset_run(message);
        break;
    case RX_IPC_VOLUME:
        if (active && message->run == app_status.run) {
            pending_volume = message->bytes;
            app_status.volume_last = pending_volume;
            app_status.volume_updates++;
            schedule_i2c();
        }
        break;
    case RX_IPC_FRAME:
        if (!active || message->run != app_status.run) break;
        while (next_sequence < message->seq) output_one_frame(next_sequence, true);
        if (message->seq == next_sequence) {
            message_q_buf = *message;
            output_one_frame(message->seq, false);
        }
        app_status.queue_peak = MAX(app_status.queue_peak,
                                    k_msgq_num_used_get(&message_q));
        break;
    case RX_IPC_END:
        if (active && message->run == app_status.run) finish_run();
        break;
    default:
        break;
    }
}

int main(void)
{
    int ipc_rc = ipc_init();
    int i2s_rc = i2s_init();
    clock_nominal = nrf_clock_hfclkaudio_config_get(NRF_CLOCK);
    printk("APP_CLOCK nominal=%u stress=%u\n", clock_nominal, AUDIO_CLOCK_STRESS);
    i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
    int codec_rc = 0;
    lc3_custom_configure(CUSTOM_SNS_COARSE, CUSTOM_VBR_ANALYTIC, 0);
    for (unsigned channel = 0; channel < APP_CHANNELS; channel++) {
        decoders[channel] = lc3_setup_decoder(2500, 48000, 48000,
                                              &decoder_memory[channel]);
        if (!decoders[channel]) codec_rc = -ENOMEM;
    }
    printk("APP_BOOT ipc=%d i2s=%d i2c_ready=%u codec=%d cpu_hz=%u\n",
           ipc_rc, i2s_rc, i2c_dev && device_is_ready(i2c_dev), codec_rc,
           SystemCoreClock);
    if (ipc_rc || codec_rc) return 0;

    while (1) {
        if (!k_msgq_get(&message_q, &message_q_buf, K_FOREVER)) {
            process_message(&message_q_buf);
        }
    }
}
