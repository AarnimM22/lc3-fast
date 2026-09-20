/* Clock-start sequence follows Nordic NCS 3.2.1 ESB sample. */
#include "bench.h"
#include <string.h>
#include <esb.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#if defined(NRF54LM20A_ENGA_XXAA)
#include <hal/nrf_clock.h>
#endif
K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_packet), 32, 4);
K_SEM_DEFINE(tx_done, 0, 1);
uint32_t rx_overflow, tx_ok, tx_fail, tx_retries;
static volatile int tx_result;
static void event(const struct esb_evt *e)
{
    if (e->evt_id == ESB_EVENT_RX_RECEIVED) {
        struct esb_payload p;
        while (!esb_read_rx_payload(&p)) {
            struct rx_packet r = {.length = p.length};
            memcpy(r.data, p.data, p.length);
            if (k_msgq_put(&rx_queue, &r, K_NO_WAIT)) { rx_overflow++; }
        }
    } else {
        tx_retries += e->tx_attempts > 0 ? e->tx_attempts - 1 : 0;
        tx_result = e->evt_id == ESB_EVENT_TX_SUCCESS ? 0 : -EIO;
        if (tx_result) { tx_fail++; } else { tx_ok++; }
        k_sem_give(&tx_done);
    }
}
int radio_init(void)
{
    struct onoff_manager *mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    static struct onoff_client cli;
    int rc, result;
    sys_notify_init_spinwait(&cli.notify);
    rc = onoff_request(mgr, &cli);
    if (rc < 0) return rc;
    do { rc = sys_notify_fetch_result(&cli.notify, &result); } while (rc == -EAGAIN);
    if (rc || result) return rc ? rc : result;
#if defined(NRF54LM20A_ENGA_XXAA)
    /* MLTPAN-39: same PLL-start workaround as the NCS 3.2.1 ESB sample. */
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_PLLSTART);
#endif
    struct esb_config c = ESB_DEFAULT_CONFIG;
    c.protocol = ESB_PROTOCOL_ESB_DPL;
    c.bitrate = ESB_BITRATE_2MBPS;
    c.mode = IS_ENABLED(BENCH_TX) ? ESB_MODE_PTX : ESB_MODE_PRX;
    c.event_handler = event;
    c.retransmit_delay = 600;
    c.retransmit_count = 3;
    c.selective_auto_ack = false;
    /* This API takes signed dBm, not the RADIO register encoding (which
     * differs on nRF54LM20A). */
    c.tx_output_power = 0;
    rc = esb_init(&c);
    uint8_t addr[4] = {0x53, 0x43, 0x31, 0x48}, prefix = 0xa7;
    if (!rc) rc = esb_set_base_address_0(addr);
    if (!rc) rc = esb_set_prefixes(&prefix, 1);
    if (!rc) rc = esb_set_rf_channel(40);
    if (!rc && !IS_ENABLED(BENCH_TX)) rc = esb_start_rx();
    return rc;
}
int radio_send(const void *data, unsigned len)
{
    struct esb_payload p = {.length = len, .pipe = 0, .noack = false};
    memcpy(p.data, data, len);
    k_sem_reset(&tx_done);
    int rc = esb_write_payload(&p);
    if (rc) return rc;
    rc = k_sem_take(&tx_done, K_MSEC(20));
    if (rc) { esb_flush_tx(); return rc; }
    if (tx_result) esb_flush_tx();
    return tx_result;
}
