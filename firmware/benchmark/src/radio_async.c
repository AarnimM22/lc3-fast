#include "bench.h"
#include <string.h>
#include <zephyr/sys/atomic.h>

#if OPT_TRANSPORT
/* Four waiting packets and one owned by the worker. */
K_MEM_SLAB_DEFINE_STATIC(tx_pool, sizeof(struct radio_owned_packet), 5, 8);
K_MSGQ_DEFINE(tx_queue, sizeof(struct radio_owned_packet *), 4, 8);
#else
struct queued_packet {
    uint64_t ready_us, deadline_us;
    uint16_t length;
    uint8_t data[252];
};
#ifdef CODEC_WAVPACK
/* Short WavPack blocks have variable sizes and occasional large bursts. */
K_MSGQ_DEFINE(tx_queue, sizeof(struct queued_packet), 16, 8);
#else
K_MSGQ_DEFINE(tx_queue, sizeof(struct queued_packet), 4, 8);
#endif
#endif
static atomic_t pending;
static struct radio_async_stats stats;

static void tx_worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (;;) {
#if OPT_TRANSPORT
        struct radio_owned_packet *owned;
        k_msgq_get(&tx_queue, &owned, K_FOREVER);
        const struct radio_owned_packet *p=owned;
#else
        struct queued_packet packet;
        k_msgq_get(&tx_queue, &packet, K_FOREVER);
        const struct queued_packet *p=&packet;
#endif
        uint64_t start = now_us();
        int rc = radio_send(p->data, p->length);
        uint64_t finish = now_us();
        uint32_t service = finish - start;
        uint32_t latency = finish - p->ready_us;
        stats.completed++;
        stats.errors += rc != 0;
        stats.late += finish > p->deadline_us;
        stats.service_total_us += service;
        stats.latency_total_us += latency;
        stats.service_max_us = MAX(stats.service_max_us, service);
        stats.latency_max_us = MAX(stats.latency_max_us, latency);
#if OPT_TRANSPORT
        k_mem_slab_free(&tx_pool, owned);
#endif
        atomic_dec(&pending);
    }
}
/* Higher priority than the encoder: brief packet setup, then block on the
 * existing ACK semaphore. RADIO/EasyDMA operate while the encoder runs. */
K_THREAD_DEFINE(tx_worker_id, 3072, tx_worker, NULL, NULL, NULL, -1, 0, 0);

int radio_async_reset(void)
{
    if (atomic_get(&pending)) return -EBUSY;
    memset(&stats, 0, sizeof(stats));
    return 0;
}

int radio_async_submit(const void *data, unsigned len, uint64_t ready_us,
                       uint64_t deadline_us)
{
#if OPT_TRANSPORT
    if (len>252) return -EMSGSIZE;
    struct radio_owned_packet *p=radio_async_acquire();
    if (!p) return -ENOMEM;
    memcpy(p->data,data,len);
    p->length=len; p->ready_us=ready_us; p->deadline_us=deadline_us;
    return radio_async_commit(p);
#else
    if (len > sizeof(((struct queued_packet *)0)->data)) return -EMSGSIZE;
    struct queued_packet p = {.ready_us=ready_us, .deadline_us=deadline_us,
                              .length=len};
    memcpy(p.data, data, len);
    /* Increment before put: put can immediately wake/preempt into worker. */
    uint32_t count = atomic_inc(&pending) + 1;
    int rc = k_msgq_put(&tx_queue, &p, K_NO_WAIT);
    if (rc) atomic_dec(&pending);
    else stats.peak_pending = MAX(stats.peak_pending, count);
    return rc;
#endif
}

#if OPT_TRANSPORT
struct radio_owned_packet *radio_async_acquire(void)
{
    struct radio_owned_packet *p=NULL;
    if (k_mem_slab_alloc(&tx_pool,(void **)&p,K_NO_WAIT)) return NULL;
    return p;
}
void radio_async_cancel(struct radio_owned_packet *p)
{
    k_mem_slab_free(&tx_pool,p);
}
int radio_async_commit(struct radio_owned_packet *p)
{
    if (p->length>sizeof(p->data)) {
        radio_async_cancel(p); return -EMSGSIZE;
    }
    uint32_t count=atomic_inc(&pending)+1;
    int rc=k_msgq_put(&tx_queue,&p,K_NO_WAIT);
    if (rc) { atomic_dec(&pending); radio_async_cancel(p); }
    else stats.peak_pending=MAX(stats.peak_pending,count);
    return rc;
}
#endif

int radio_async_drain(struct radio_async_stats *out)
{
    uint64_t deadline = now_us() + 200000;
    while (atomic_get(&pending)) {
        if (now_us() > deadline) return -ETIMEDOUT;
        k_sleep(K_USEC(100));
    }
    *out = stats;
    return 0;
}
