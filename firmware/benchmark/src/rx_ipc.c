#include "rx_ipc.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>

static struct ipc_ept endpoint;
static K_SEM_DEFINE(bound_sem, 0, 1);
static K_SEM_DEFINE(status_sem, 0, 1);
static volatile bool endpoint_bound;
static struct rx_app_status last_status;

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
    if (len == sizeof(last_status)) {
        struct rx_app_status status;
        memcpy(&status, data, sizeof(status));
        if (status.magic == RX_IPC_MAGIC) {
            last_status = status;
            k_sem_give(&status_sem);
        }
    }
}

static void endpoint_error_cb(const char *message, void *priv)
{
    ARG_UNUSED(priv);
    printk("RX_IPC_ERROR %s\n", message ? message : "unknown");
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

int rx_ipc_init(void)
{
    const struct device *instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));
    if (!device_is_ready(instance)) {
        printk("RX_IPC_DEVICE_NOT_READY\n");
        return -ENODEV;
    }

    int rc = ipc_service_open_instance(instance);
    printk("RX_IPC_OPEN rc=%d\n", rc);
    if (rc < 0 && rc != -EALREADY) return rc;
    rc = ipc_service_register_endpoint(instance, &endpoint, &endpoint_cfg);
    printk("RX_IPC_REGISTER rc=%d\n", rc);
    if (rc < 0) return rc;
    rc = k_sem_take(&bound_sem, K_MSEC(3000));
    if (rc) {
        printk("RX_IPC_BIND_TIMEOUT rc=%d\n", rc);
        return rc;
    }
    printk("RX_IPC_BOUND backend=rpmsg\n");
    return 0;
}

static int send_message(const struct rx_ipc_message *message, size_t length)
{
    if (!endpoint_bound) return -ENOTCONN;
    /* RPMsg can briefly run out of free vring buffers while the application
     * core is decoding.  Retry for a bounded interval instead of dropping a
     * frame silently.  This runs on the network-core thread, outside the
     * RADIO interrupt/ACK path. */
    for (unsigned attempt = 0; attempt < 200; attempt++) {
        int rc = ipc_service_send(&endpoint, message, length);
        if (rc >= 0) return 0;
        if (rc != -ENOMEM) return rc;
        k_busy_wait(25);
    }
    return -EAGAIN;
}

static void init_message(struct rx_ipc_message *message, uint32_t run,
                         uint32_t seq, uint8_t type)
{
    memset(message, 0, sizeof(*message));
    message->magic = RX_IPC_MAGIC;
    message->run = run;
    message->seq = seq;
    message->type = type;
}

int rx_ipc_send_start(uint32_t run, uint32_t frames)
{
    struct rx_ipc_message message;
    init_message(&message, run, frames, RX_IPC_START);
    return send_message(&message, offsetof(struct rx_ipc_message, data));
}

int rx_ipc_send_frame(uint32_t run, uint32_t seq, const uint8_t *data,
                      unsigned bytes, uint32_t crc)
{
    if (!data || bytes > RX_IPC_MAX_FRAME) return -EMSGSIZE;
    struct rx_ipc_message message;
    init_message(&message, run, seq, RX_IPC_FRAME);
    message.bytes = bytes;
    message.crc = crc;
    memcpy(message.data, data, bytes);
    return send_message(&message, offsetof(struct rx_ipc_message, data) + bytes);
}

int rx_ipc_send_end(uint32_t run, uint32_t frames)
{
    k_sem_reset(&status_sem);
    struct rx_ipc_message message;
    init_message(&message, run, frames, RX_IPC_END);
    return send_message(&message, offsetof(struct rx_ipc_message, data));
}

int rx_ipc_send_volume(uint32_t run, uint32_t seq, uint16_t level)
{
    struct rx_ipc_message message;
    init_message(&message, run, seq, RX_IPC_VOLUME);
    message.bytes = level;
    return send_message(&message, offsetof(struct rx_ipc_message, data));
}

int rx_ipc_wait_status(struct rx_app_status *status, int timeout_ms)
{
    if (!status) return -EINVAL;
    int rc = k_sem_take(&status_sem, K_MSEC(timeout_ms));
    if (!rc) *status = last_status;
    return rc;
}
