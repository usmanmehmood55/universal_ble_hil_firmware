#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(universal_ble_hil, LOG_LEVEL_INF);

#define HIL_MAX_VALUE_LENGTH 512
#define HIL_MAX_WRITE_LENGTH 384
#define HIL_MAX_NOTIFICATION_LENGTH 244
#define HIL_MAX_SCRIPTED_NOTIFICATIONS 64
#define HIL_CONTRACT_REVISION 2
#define HIL_NO_DISCONNECT UINT16_MAX

enum hil_command
{
    HIL_COMMAND_RESET           = 0x01,
    HIL_COMMAND_SET_READ_VALUE  = 0x02,
    HIL_COMMAND_NOTIFY          = 0x03,
    HIL_COMMAND_INDICATE        = 0x04,
    HIL_COMMAND_DISCONNECT      = 0x05,
    HIL_COMMAND_NOTIFY_BURST    = 0x06,
    HIL_COMMAND_ARM_READ_FAULT  = 0x07,
    HIL_COMMAND_ARM_WRITE_FAULT = 0x08,
    HIL_COMMAND_NOTIFY_SCRIPT   = 0x09,
    HIL_COMMAND_NOTIFY_ON_SUBSCRIBE = 0x0a,
    HIL_COMMAND_SET_AUXILIARY_SERVICE = 0x0b,
    HIL_COMMAND_SCHEDULE_AUXILIARY_SERVICE = 0x0c,
    HIL_COMMAND_ARM_CCC_DELAY = 0x0d,
    HIL_COMMAND_ARM_DESCRIPTOR_READ_FAULT = 0x0e,
    HIL_COMMAND_ARM_DESCRIPTOR_WRITE_FAULT = 0x0f,
    HIL_COMMAND_INDICATE_ON_SUBSCRIBE = 0x10,
    HIL_COMMAND_INDICATION_SCENARIO = 0x11,
    HIL_COMMAND_ARM_CCC_FAULT = 0x12,
    HIL_COMMAND_SET_LONG_VALUE = 0x13,
    HIL_COMMAND_NOTIFY_PRESSURE = 0x14,
    HIL_COMMAND_ARM_CONNECT_FAULT = 0x15,
    HIL_COMMAND_ARM_DISCOVERY_FAULT = 0x16,
};

struct hil_operation_fault
{
    uint8_t att_error;
    uint16_t delay_ms;
    uint16_t disconnect_after_ms;
    bool armed;
};

struct hil_state
{
    uint8_t read_value[HIL_MAX_VALUE_LENGTH];
    uint16_t read_length;
    uint8_t write_value[HIL_MAX_VALUE_LENGTH];
    uint16_t write_length;
    uint8_t write_without_response_value[HIL_MAX_VALUE_LENGTH];
    uint16_t write_without_response_length;
    uint8_t multi_value[HIL_MAX_VALUE_LENGTH];
    uint16_t multi_length;
    uint8_t descriptor_value[HIL_MAX_VALUE_LENGTH];
    uint16_t descriptor_length;
    uint32_t writes_with_response;
    uint32_t writes_without_response;
    bool notify_enabled;
    bool indicate_enabled;
    bool multi_notify_enabled;
};

static struct hil_state state;
static struct bt_conn *active_connection;
static struct k_work_delayable advertising_work;
static struct k_work_delayable disconnect_work;
static struct k_spinlock disconnect_lock;
static struct bt_conn *disconnect_connection;
static int64_t disconnect_deadline_ms;
static atomic_t disconnect_timers_armed;
static atomic_t disconnect_timers_attempted;
static atomic_t disconnect_timers_cancelled;
static struct k_work_delayable burst_work;
static struct k_work_delayable auxiliary_service_work;
static uint16_t burst_remaining;
static uint16_t burst_interval_ms;
static uint16_t burst_payload_size;
static uint16_t burst_sequence;
static uint16_t burst_script[HIL_MAX_SCRIPTED_NOTIFICATIONS];
static uint16_t burst_script_index;
static bool burst_uses_script;
static struct bt_gatt_indicate_params indication_params;
static uint8_t indication_value[HIL_MAX_VALUE_LENGTH];
static struct hil_operation_fault read_fault;
static struct hil_operation_fault write_fault;
static struct hil_operation_fault descriptor_read_fault;
static struct hil_operation_fault descriptor_write_fault;
static bool notify_on_subscribe_armed;
static bool auxiliary_service_requested;
static uint16_t ccc_delay_ms;
static bool indicate_on_subscribe_armed;
static atomic_t indication_pending;
static atomic_t indication_completed;
static atomic_t indication_rejected;
static atomic_t indication_fault_triggered;
static atomic_t ccc_fault_triggered;
static uint8_t ccc_fault_action;
static uint16_t ccc_fault_delay_ms;
static uint16_t ccc_fault_after_ms;
static struct k_work_delayable reboot_work;
static struct k_work_delayable pressure_work;
static atomic_t oversized_writes_rejected;
static atomic_t pressure_accepted;
static atomic_t pressure_rejected;
static atomic_t pressure_completed;
static atomic_t pressure_errors;
static atomic_t connect_fault_triggered;
static bool connect_fault_armed;
static uint16_t connect_fault_delay_ms;
static struct hil_operation_fault discovery_fault;
static atomic_t discovery_fault_triggered;
static atomic_t physical_disconnects;
static __noinit uint32_t boot_magic;
static __noinit uint32_t boot_count;

#define HIL_UUID(value) BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x7e570000 + (value), 0x7e57, 0x4e57, 0x8e57, 0x7e5700000001))

static struct bt_uuid_128 service_uuid                       = HIL_UUID(0x01);
static struct bt_uuid_128 control_uuid                       = HIL_UUID(0x02);
static struct bt_uuid_128 state_uuid                         = HIL_UUID(0x03);
static struct bt_uuid_128 read_uuid                          = HIL_UUID(0x04);
static struct bt_uuid_128 write_uuid                         = HIL_UUID(0x05);
static struct bt_uuid_128 write_without_response_uuid        = HIL_UUID(0x06);
static struct bt_uuid_128 write_mirror_uuid                  = HIL_UUID(0x07);
static struct bt_uuid_128 write_without_response_mirror_uuid = HIL_UUID(0x08);
static struct bt_uuid_128 notify_uuid                        = HIL_UUID(0x09);
static struct bt_uuid_128 indicate_uuid                      = HIL_UUID(0x0a);
static struct bt_uuid_128 multi_uuid                         = HIL_UUID(0x0b);
static struct bt_uuid_128 auxiliary_service_uuid             = HIL_UUID(0x0c);
static struct bt_uuid_128 auxiliary_read_uuid                = HIL_UUID(0x0d);
static struct bt_uuid_128 descriptor_uuid                    = HIL_UUID(0x0e);
static struct bt_uuid_128 telemetry_uuid                     = HIL_UUID(0x0f);

static ssize_t read_auxiliary(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr, void *buf,
                              uint16_t len, uint16_t offset)
{
    static const uint8_t value[] = "AUXILIARY-V1";
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
                             sizeof(value) - 1);
}

static struct bt_gatt_attr auxiliary_service_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(&auxiliary_service_uuid),
    BT_GATT_CHARACTERISTIC(&auxiliary_read_uuid.uuid,
                           BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
                           read_auxiliary, NULL, NULL),
};

static struct bt_gatt_service auxiliary_service =
    BT_GATT_SERVICE(auxiliary_service_attrs);

static void auxiliary_service_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    bool registered = bt_gatt_service_is_registered(&auxiliary_service);
    int err = 0;

    if (auxiliary_service_requested && !registered)
    {
        err = bt_gatt_service_register(&auxiliary_service);
    }
    else if (!auxiliary_service_requested && registered)
    {
        err = bt_gatt_service_unregister(&auxiliary_service);
    }

    if (err != 0)
    {
        LOG_ERR("Failed to %s auxiliary service (err %d)",
                auxiliary_service_requested ? "register" : "unregister", err);
    }
}

static void cancel_disconnect(struct bt_conn *expected)
{
    k_spinlock_key_t key = k_spin_lock(&disconnect_lock);
    struct bt_conn *target = disconnect_connection;
    if (target != NULL && (expected == NULL || target == expected))
    {
        disconnect_connection = NULL;
        k_work_cancel_delayable(&disconnect_work);
        atomic_inc(&disconnect_timers_cancelled);
    }
    else
    {
        target = NULL;
    }
    k_spin_unlock(&disconnect_lock, key);
    if (target != NULL) bt_conn_unref(target);
}

static void schedule_disconnect(uint16_t delay_ms)
{
    k_spinlock_key_t key = k_spin_lock(&disconnect_lock);
    struct bt_conn *previous = disconnect_connection;
    disconnect_connection = active_connection != NULL ? bt_conn_ref(active_connection) : NULL;
    if (disconnect_connection != NULL)
    {
        disconnect_deadline_ms = k_uptime_get() + delay_ms;
        atomic_inc(&disconnect_timers_armed);
        k_work_reschedule(&disconnect_work, K_MSEC(delay_ms));
    }
    k_spin_unlock(&disconnect_lock, key);
    if (previous != NULL) bt_conn_unref(previous);
}

static void reset_state(void)
{
    static const uint8_t initial_read_value[] = "HIL-READ-V1";
    static const uint8_t initial_multi_value[] = "MULTI-V1";
    static const uint8_t initial_descriptor_value[] = "HIL-DESCRIPTOR-V1";
    bool notify_enabled = state.notify_enabled;
    bool indicate_enabled = state.indicate_enabled;
    bool multi_notify_enabled = state.multi_notify_enabled;

    cancel_disconnect(NULL);
    k_work_cancel_delayable(&burst_work);
    k_work_cancel_delayable(&reboot_work);
    k_work_cancel_delayable(&pressure_work);
    memset(&state, 0, sizeof(state));
    memcpy(state.read_value, initial_read_value, sizeof(initial_read_value) - 1);
    state.read_length = sizeof(initial_read_value) - 1;
    memcpy(state.multi_value, initial_multi_value, sizeof(initial_multi_value) - 1);
    state.multi_length = sizeof(initial_multi_value) - 1;
    memcpy(state.descriptor_value, initial_descriptor_value,
           sizeof(initial_descriptor_value) - 1);
    state.descriptor_length = sizeof(initial_descriptor_value) - 1;
    state.notify_enabled = notify_enabled;
    state.indicate_enabled = indicate_enabled;
    state.multi_notify_enabled = multi_notify_enabled;
    burst_remaining = 0;
    burst_sequence = 0;
    burst_script_index = 0;
    burst_uses_script = false;
    memset(&read_fault, 0, sizeof(read_fault));
    memset(&write_fault, 0, sizeof(write_fault));
    memset(&descriptor_read_fault, 0, sizeof(descriptor_read_fault));
    memset(&descriptor_write_fault, 0, sizeof(descriptor_write_fault));
    notify_on_subscribe_armed = false;
    auxiliary_service_requested = false;
    ccc_delay_ms = 0;
    indicate_on_subscribe_armed = false;
    ccc_fault_action = 0;
    atomic_set(&indication_completed, 0);
    atomic_set(&indication_rejected, 0);
    atomic_set(&indication_fault_triggered, 0);
    atomic_set(&ccc_fault_triggered, 0);
    atomic_set(&oversized_writes_rejected, 0);
    atomic_set(&pressure_accepted, 0);
    atomic_set(&pressure_rejected, 0);
    atomic_set(&pressure_completed, 0);
    atomic_set(&pressure_errors, 0);
    atomic_set(&connect_fault_triggered, 0);
    connect_fault_armed = false;
    memset(&discovery_fault, 0, sizeof(discovery_fault));
    atomic_set(&discovery_fault_triggered, 0);
    atomic_set(&physical_disconnects, 0);
    atomic_set(&disconnect_timers_armed, 0);
    atomic_set(&disconnect_timers_attempted, 0);
    atomic_set(&disconnect_timers_cancelled, 0);
}

static ssize_t apply_operation_fault(struct hil_operation_fault *fault)
{
    if (!fault->armed)
    {
        return 0;
    }

    struct hil_operation_fault active_fault = *fault;
    memset(fault, 0, sizeof(*fault));

    if (active_fault.disconnect_after_ms != HIL_NO_DISCONNECT)
    {
        schedule_disconnect(active_fault.disconnect_after_ms);
    }
    if (active_fault.delay_ms > 0)
    {
        k_sleep(K_MSEC(active_fault.delay_ms));
    }
    if (active_fault.att_error != 0)
    {
        return BT_GATT_ERR(active_fault.att_error);
    }
    return 0;
}

static ssize_t read_buffer(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset,
                           const void *value, uint16_t value_length)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, value_length);
}

static ssize_t read_state(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset)
{
    uint8_t value[16] = {0};
    value[0] = HIL_CONTRACT_REVISION;
    value[1] = state.notify_enabled ? 1 : 0;
    value[2] = state.indicate_enabled ? 1 : 0;
    value[3] = state.multi_notify_enabled ? 1 : 0;
    sys_put_le32(state.writes_with_response, &value[4]);
    sys_put_le32(state.writes_without_response, &value[8]);
    sys_put_le16(bt_gatt_get_mtu(conn), &value[12]);
    return read_buffer(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t read_value(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset)
{
    ssize_t fault_result = apply_operation_fault(&read_fault);
    if (fault_result < 0)
    {
        return fault_result;
    }
    return read_buffer(conn, attr, buf, len, offset, state.read_value, state.read_length);
}

static ssize_t read_hil_service(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    if (discovery_fault.armed) atomic_inc(&discovery_fault_triggered);
    ssize_t result = apply_operation_fault(&discovery_fault);
    if (result < 0) return result;
    return bt_gatt_attr_read_service(conn, attr, buf, len, offset);
}

static ssize_t read_telemetry(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset)
{
    uint8_t value[36];
    sys_put_le16(atomic_get(&indication_pending), &value[0]);
    sys_put_le16(atomic_get(&indication_completed), &value[2]);
    sys_put_le16(atomic_get(&indication_rejected), &value[4]);
    sys_put_le16(atomic_get(&indication_fault_triggered), &value[6]);
    sys_put_le16(atomic_get(&ccc_fault_triggered), &value[8]);
    sys_put_le32(boot_count, &value[10]);
    sys_put_le16(atomic_get(&oversized_writes_rejected), &value[14]);
    sys_put_le16(atomic_get(&pressure_accepted), &value[16]);
    sys_put_le16(atomic_get(&pressure_rejected), &value[18]);
    sys_put_le16(atomic_get(&pressure_completed), &value[20]);
    sys_put_le16(atomic_get(&pressure_errors), &value[22]);
    sys_put_le16(atomic_get(&connect_fault_triggered), &value[24]);
    sys_put_le16(atomic_get(&discovery_fault_triggered), &value[26]);
    sys_put_le16(atomic_get(&physical_disconnects), &value[28]);
    sys_put_le16(atomic_get(&disconnect_timers_armed), &value[30]);
    sys_put_le16(atomic_get(&disconnect_timers_attempted), &value[32]);
    sys_put_le16(atomic_get(&disconnect_timers_cancelled), &value[34]);
    return read_buffer(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t read_write_mirror(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset)
{
    return read_buffer(conn, attr, buf, len, offset, state.write_value, state.write_length);
}

static ssize_t store_write(const void *buf, uint16_t len, uint16_t offset,
                           uint8_t *target, uint16_t *target_length);

static ssize_t read_descriptor_value(struct bt_conn *conn,
                                     const struct bt_gatt_attr *attr,
                                     void *buf, uint16_t len, uint16_t offset)
{
    ssize_t fault_result = apply_operation_fault(&descriptor_read_fault);
    if (fault_result < 0)
    {
        return fault_result;
    }
    return read_buffer(conn, attr, buf, len, offset, state.descriptor_value,
                       state.descriptor_length);
}

static ssize_t write_descriptor_value(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      const void *buf, uint16_t len,
                                      uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    ssize_t fault_result = apply_operation_fault(&descriptor_write_fault);
    if (fault_result < 0)
    {
        return fault_result;
    }
    return store_write(buf, len, offset, state.descriptor_value,
                       &state.descriptor_length);
}

static ssize_t read_write_without_response_mirror(struct bt_conn *conn,
                                                  const struct bt_gatt_attr *attr,
                                                  void *buf, uint16_t len,
                                                  uint16_t offset)
{
    return read_buffer(conn, attr, buf, len, offset,
                       state.write_without_response_value,
                       state.write_without_response_length);
}

static ssize_t read_multi(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset)
{
    return read_buffer(conn, attr, buf, len, offset, state.multi_value, state.multi_length);
}

static ssize_t store_write(const void *buf, uint16_t len, uint16_t offset,
                           uint8_t *target, uint16_t *target_length)
{
    if (offset > HIL_MAX_VALUE_LENGTH || len > HIL_MAX_VALUE_LENGTH - offset)
    {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    memcpy(target + offset, buf, len);
    *target_length = offset + len;
    return len;
}

static ssize_t write_value(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset,
                           uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    ssize_t fault_result = apply_operation_fault(&write_fault);
    if (fault_result < 0)
    {
        return fault_result;
    }
    if (offset > HIL_MAX_WRITE_LENGTH || len > HIL_MAX_WRITE_LENGTH - offset)
    {
        atomic_inc(&oversized_writes_rejected);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (flags & BT_GATT_WRITE_FLAG_PREPARE)
    {
        return 0;
    }
    ssize_t result = store_write(buf, len, offset, state.write_value, &state.write_length);
    if (result >= 0)
    {
        state.writes_with_response++;
    }
    return result;
}

static ssize_t write_without_response_value(struct bt_conn *conn,
                                            const struct bt_gatt_attr *attr,
                                            const void *buf, uint16_t len,
                                            uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    ssize_t result = store_write(buf, len, offset,
                                 state.write_without_response_value,
                                 &state.write_without_response_length);
    if (result >= 0)
    {
        state.writes_without_response++;
    }
    return result;
}

static ssize_t write_multi(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset,
                           uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    return store_write(buf, len, offset, state.multi_value, &state.multi_length);
}

static void disconnect_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&disconnect_lock);
    // Cancellation is asynchronous: an old invocation may already be running
    // when a newer plan is armed. It must respect the newer plan's deadline.
    int64_t remaining_ms = disconnect_deadline_ms - k_uptime_get();
    if (disconnect_connection != NULL && remaining_ms > 0)
    {
        k_work_reschedule(&disconnect_work, K_MSEC(remaining_ms));
        k_spin_unlock(&disconnect_lock, key);
        return;
    }
    struct bt_conn *target = disconnect_connection;
    disconnect_connection = NULL;
    k_spin_unlock(&disconnect_lock, key);
    if (target != NULL)
    {
        atomic_inc(&disconnect_timers_attempted);
        int err = bt_conn_disconnect(target, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if (err != 0 && err != -ENOTCONN)
        {
            LOG_ERR("Failed to disconnect (err %d)", err);
        }
        bt_conn_unref(target);
    }
}

static const struct bt_gatt_attr *notify_attribute(void);
static const struct bt_gatt_attr *indicate_attribute(void);

static int send_notification(const uint8_t *value, uint16_t length)
{
    if (active_connection == NULL)
    {
        return -ENOTCONN;
    }
    return bt_gatt_notify(active_connection, notify_attribute(), value, length);
}

static void pressure_notify_complete(struct bt_conn *conn, void *user_data)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(user_data);
    atomic_inc(&pressure_completed);
}

static void pressure_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (active_connection == NULL) return;
    static const uint8_t value[] = "PRESSURE";
    struct bt_gatt_notify_params params = {
        .attr = notify_attribute(), .data = value, .len = sizeof(value) - 1,
        .func = pressure_notify_complete,
    };
    // Completion callbacks use this same workqueue. This loop cannot release
    // their resources until it returns; allocation here is nonblocking.
    for (int i = 0; i < 128; ++i)
    {
        int err = bt_gatt_notify_cb(active_connection, &params);
        if (err == 0) atomic_inc(&pressure_accepted);
        else if (err == -ENOMEM) atomic_inc(&pressure_rejected);
        else atomic_inc(&pressure_errors);
    }
}

static void indication_complete(struct bt_conn *conn,
                                struct bt_gatt_indicate_params *params,
                                uint8_t err)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(params);
    LOG_DBG("Indication completed (err 0x%02x)", err);
    if (err == 0) atomic_inc(&indication_completed);
}

static void indication_destroy(struct bt_gatt_indicate_params *params)
{
    ARG_UNUSED(params);
    atomic_clear(&indication_pending);
}

static void reboot_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    sys_reboot(SYS_REBOOT_COLD);
}

static int send_indication(const uint8_t *value, uint16_t length)
{
    if (active_connection == NULL || length > sizeof(indication_value))
    {
        return -ENOTCONN;
    }
    if (!atomic_cas(&indication_pending, 0, 1))
    {
        atomic_inc(&indication_rejected);
        return -EBUSY;
    }
    memcpy(indication_value, value, length);
    memset(&indication_params, 0, sizeof(indication_params));
    indication_params.attr = indicate_attribute();
    indication_params.func = indication_complete;
    indication_params.destroy = indication_destroy;
    indication_params.data = indication_value;
    indication_params.len  = length;
    int err = bt_gatt_indicate(active_connection, &indication_params);
    if (err != 0) atomic_clear(&indication_pending);
    return err;
}

static void burst_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (burst_remaining == 0 || active_connection == NULL)
    {
        return;
    }

    uint8_t payload[HIL_MAX_VALUE_LENGTH];
    uint16_t sequence = burst_uses_script
                            ? burst_script[burst_script_index]
                            : burst_sequence;
    sys_put_le16(sequence, payload);
    for (uint16_t index = 2; index < burst_payload_size; index++)
    {
        payload[index] = (uint8_t)index;
    }
    int err = send_notification(payload, burst_payload_size);
    if (err == -ENOMEM || err == -EAGAIN)
    {
        k_work_reschedule(&burst_work, K_MSEC(5));
        return;
    }
    if (err != 0)
    {
        LOG_ERR("Burst notification failed (err %d)", err);
        burst_remaining = 0;
        return;
    }

    if (burst_uses_script)
    {
        burst_script_index++;
    }
    else
    {
        burst_sequence++;
    }
    burst_remaining--;
    if (burst_remaining > 0)
    {
        k_work_reschedule(&burst_work, K_MSEC(burst_interval_ms));
    }
}

static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset,
                             uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    if (offset != 0 || len == 0)
    {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    const uint8_t *command = buf;
    const uint8_t *payload = command + 1;
    uint16_t payload_length = len - 1;

    switch (command[0])
    {
    case HIL_COMMAND_RESET:
        reset_state();
        k_work_reschedule(&auxiliary_service_work, K_MSEC(25));
        break;
    case HIL_COMMAND_SET_READ_VALUE:
        if (payload_length > HIL_MAX_VALUE_LENGTH)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        memcpy(state.read_value, payload, payload_length);
        state.read_length = payload_length;
        break;
    case HIL_COMMAND_NOTIFY:
        if (!state.notify_enabled || send_notification(payload, payload_length) != 0)
        {
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
        break;
    case HIL_COMMAND_INDICATE:
        if (!state.indicate_enabled || send_indication(payload, payload_length) != 0)
        {
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
        break;
    case HIL_COMMAND_DISCONNECT:
        if (payload_length != 2)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        schedule_disconnect(sys_get_le16(payload));
        break;
    case HIL_COMMAND_NOTIFY_BURST:
        if (payload_length != 6 || !state.notify_enabled)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        burst_remaining = sys_get_le16(&payload[0]);
        burst_payload_size = sys_get_le16(&payload[2]);
        burst_interval_ms = sys_get_le16(&payload[4]);
        if (burst_remaining == 0 || burst_payload_size < 2 ||
            burst_payload_size > HIL_MAX_NOTIFICATION_LENGTH)
        {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        burst_sequence = 0;
        burst_script_index = 0;
        burst_uses_script = false;
        k_work_reschedule(&burst_work, K_NO_WAIT);
        break;
    case HIL_COMMAND_NOTIFY_SCRIPT:
    {
        if (payload_length < 5 || !state.notify_enabled)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        uint8_t count = payload[4];
        if (count == 0 || count > HIL_MAX_SCRIPTED_NOTIFICATIONS ||
            payload_length != 5 + (uint16_t)count * sizeof(uint16_t))
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        burst_payload_size = sys_get_le16(&payload[0]);
        burst_interval_ms = sys_get_le16(&payload[2]);
        if (burst_payload_size < 2 ||
            burst_payload_size > HIL_MAX_NOTIFICATION_LENGTH)
        {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        for (uint8_t index = 0; index < count; index++)
        {
            burst_script[index] = sys_get_le16(&payload[5 + index * 2]);
        }
        burst_remaining = count;
        burst_script_index = 0;
        burst_uses_script = true;
        k_work_reschedule(&burst_work, K_NO_WAIT);
        break;
    }
    case HIL_COMMAND_NOTIFY_ON_SUBSCRIBE:
        if (payload_length != 0)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        notify_on_subscribe_armed = true;
        break;
    case HIL_COMMAND_SET_AUXILIARY_SERVICE:
        if (payload_length != 1 || payload[0] > 1)
        {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        auxiliary_service_requested = payload[0] != 0;
        k_work_reschedule(&auxiliary_service_work, K_MSEC(25));
        break;
    case HIL_COMMAND_SCHEDULE_AUXILIARY_SERVICE:
        if (payload_length != 3 || payload[0] > 1)
        {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        auxiliary_service_requested = payload[0] != 0;
        k_work_reschedule(&auxiliary_service_work,
                          K_MSEC(sys_get_le16(&payload[1])));
        break;
    case HIL_COMMAND_ARM_CCC_DELAY:
        if (payload_length != 2)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        ccc_delay_ms = sys_get_le16(payload);
        break;
    case HIL_COMMAND_INDICATE_ON_SUBSCRIBE:
        if (payload_length != 0) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        indicate_on_subscribe_armed = true;
        break;
    case HIL_COMMAND_SET_LONG_VALUE:
        if (payload_length != 2 || sys_get_le16(payload) > HIL_MAX_VALUE_LENGTH)
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        state.read_length = sys_get_le16(payload);
        for (uint16_t i = 0; i < state.read_length; ++i) state.read_value[i] = (uint8_t)i;
        break;
    case HIL_COMMAND_NOTIFY_PRESSURE:
        if (payload_length != 0 || !state.notify_enabled)
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        if (atomic_get(&pressure_accepted) || atomic_get(&pressure_rejected))
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        k_work_reschedule(&pressure_work, K_MSEC(25));
        break;
    case HIL_COMMAND_ARM_CONNECT_FAULT:
        if (payload_length != 2) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        connect_fault_delay_ms = sys_get_le16(payload);
        connect_fault_armed = true;
        break;
    case HIL_COMMAND_ARM_CCC_FAULT:
        if (payload_length != 5 || payload[0] < 1 || payload[0] > 2)
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        ccc_fault_action = payload[0];
        ccc_fault_after_ms = sys_get_le16(&payload[1]);
        ccc_fault_delay_ms = sys_get_le16(&payload[3]);
        break;
    case HIL_COMMAND_INDICATION_SCENARIO:
    {
        if (payload_length != 1 || payload[0] < 1 || payload[0] > 3 || !state.indicate_enabled)
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        static const uint8_t first[] = "IND-FIRST";
        static const uint8_t second[] = "IND-SECOND";
        if (send_indication(first, sizeof(first) - 1) != 0)
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        // This RX callback cannot process the confirmation until it returns.
        if (!atomic_get(&indication_pending)) return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        atomic_inc(&indication_fault_triggered);
        if (payload[0] == 1) bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        else if (payload[0] == 2) sys_reboot(SYS_REBOOT_COLD);
        else if (send_indication(second, sizeof(second) - 1) != -EBUSY)
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        break;
    }
    case HIL_COMMAND_ARM_READ_FAULT:
    case HIL_COMMAND_ARM_DISCOVERY_FAULT:
    case HIL_COMMAND_ARM_WRITE_FAULT:
    case HIL_COMMAND_ARM_DESCRIPTOR_READ_FAULT:
    case HIL_COMMAND_ARM_DESCRIPTOR_WRITE_FAULT:
    {
        if (payload_length != 5)
        {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        struct hil_operation_fault *fault;
        if (command[0] == HIL_COMMAND_ARM_DISCOVERY_FAULT)
        {
            fault = &discovery_fault;
        }
        else if (command[0] == HIL_COMMAND_ARM_READ_FAULT)
        {
            fault = &read_fault;
        }
        else if (command[0] == HIL_COMMAND_ARM_WRITE_FAULT)
        {
            fault = &write_fault;
        }
        else if (command[0] == HIL_COMMAND_ARM_DESCRIPTOR_READ_FAULT)
        {
            fault = &descriptor_read_fault;
        }
        else
        {
            fault = &descriptor_write_fault;
        }
        fault->att_error = payload[0];
        fault->delay_ms = sys_get_le16(&payload[1]);
        fault->disconnect_after_ms = sys_get_le16(&payload[3]);
        fault->armed = true;
        break;
    }
    default:
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    return len;
}

static void notify_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    if (ccc_fault_action != 0)
    {
        uint8_t action = ccc_fault_action;
        ccc_fault_action = 0;
        atomic_inc(&ccc_fault_triggered);
        if (action == 1) schedule_disconnect(ccc_fault_after_ms);
        else k_work_reschedule(&reboot_work, K_MSEC(ccc_fault_after_ms));
        k_sleep(K_MSEC(ccc_fault_delay_ms));
    }
    if (ccc_delay_ms > 0)
    {
        uint16_t delay_ms = ccc_delay_ms;
        ccc_delay_ms = 0;
        k_sleep(K_MSEC(delay_ms));
    }
    state.notify_enabled = value == BT_GATT_CCC_NOTIFY;
    if (state.notify_enabled && notify_on_subscribe_armed)
    {
        static const uint8_t immediate_value[] = "CCC-ENABLED";

        notify_on_subscribe_armed = false;
        int err = send_notification(immediate_value,
                                    sizeof(immediate_value) - 1);
        if (err != 0)
        {
            LOG_ERR("Failed to send immediate CCC notification (err %d)", err);
        }
    }
}

static void indicate_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    state.indicate_enabled = value == BT_GATT_CCC_INDICATE;
    if (state.indicate_enabled && indicate_on_subscribe_armed)
    {
        static const uint8_t value[] = "IND-CCC-ENABLED";
        indicate_on_subscribe_armed = false;
        int err = send_indication(value, sizeof(value) - 1);
        if (err != 0) LOG_ERR("Immediate indication failed (%d)", err);
    }
}

static void multi_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    state.multi_notify_enabled = value == BT_GATT_CCC_NOTIFY;
}

BT_GATT_SERVICE_DEFINE(hil_service,
                       BT_GATT_ATTRIBUTE(BT_UUID_GATT_PRIMARY, BT_GATT_PERM_READ,
                                         read_hil_service, NULL, &service_uuid),
                       BT_GATT_CHARACTERISTIC(&control_uuid.uuid, BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_WRITE, NULL, write_control, NULL),
                       BT_GATT_CHARACTERISTIC(&state_uuid.uuid, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ, read_state, NULL, NULL),
                       BT_GATT_CHARACTERISTIC(&read_uuid.uuid, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ, read_value, NULL, NULL),
                       BT_GATT_DESCRIPTOR(BT_UUID_GATT_CUD, BT_GATT_PERM_READ,
                                           bt_gatt_attr_read_cud, NULL, "Deterministic read value"),
                       BT_GATT_DESCRIPTOR(&descriptor_uuid.uuid,
                                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                                          read_descriptor_value,
                                          write_descriptor_value, NULL),
                       BT_GATT_CHARACTERISTIC(&write_uuid.uuid, BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE,
                                              NULL, write_value, NULL),
                       BT_GATT_CHARACTERISTIC(&write_without_response_uuid.uuid,
                                              BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE,
                                              NULL, write_without_response_value, NULL),
                       BT_GATT_CHARACTERISTIC(&write_mirror_uuid.uuid, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ, read_write_mirror, NULL, NULL),
                       BT_GATT_CHARACTERISTIC(&write_without_response_mirror_uuid.uuid,
                                              BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
                                              read_write_without_response_mirror, NULL, NULL),
                       BT_GATT_CHARACTERISTIC(&notify_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_NONE, NULL, NULL, NULL),
                       BT_GATT_CCC(notify_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                       BT_GATT_CHARACTERISTIC(&indicate_uuid.uuid, BT_GATT_CHRC_INDICATE,
                                              BT_GATT_PERM_NONE, NULL, NULL, NULL),
                       BT_GATT_CCC(indicate_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                       BT_GATT_CHARACTERISTIC(&multi_uuid.uuid,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                                              read_multi, write_multi, NULL),
                       BT_GATT_CCC(multi_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                       BT_GATT_CHARACTERISTIC(&telemetry_uuid.uuid, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ, read_telemetry, NULL, NULL));

static const struct bt_gatt_attr *notify_attribute(void)
{
    return &hil_service.attrs[18];
}

static const struct bt_gatt_attr *indicate_attribute(void)
{
    return &hil_service.attrs[21];
}

static const struct bt_data advertising_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_128_ENCODE(0x7e570001, 0x7e57, 0x4e57, 0x8e57, 0x7e5700000001)),
    BT_DATA_BYTES(BT_DATA_MANUFACTURER_DATA, 0x57, 0x7e,
                  HIL_CONTRACT_REVISION, 0x00),
};

static const struct bt_data scan_response_data[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static int start_advertising(void)
{
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising_data,
                              ARRAY_SIZE(advertising_data), scan_response_data,
                              ARRAY_SIZE(scan_response_data));
    return err == -EALREADY ? 0 : err;
}

static void advertising_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    int err = start_advertising();
    if (err != 0)
    {
        LOG_ERR("Failed to restart advertising (err %d); retrying", err);
        k_work_reschedule(&advertising_work, K_MSEC(250));
        return;
    }
    LOG_INF("Advertising restarted");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err != 0)
    {
        LOG_ERR("Connection failed (err 0x%02x)", err);
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&disconnect_lock);
    struct bt_conn *previous = active_connection;
    active_connection = bt_conn_ref(conn);
    k_spin_unlock(&disconnect_lock, key);
    if (previous != NULL)
    {
        bt_conn_unref(previous);
    }
    if (connect_fault_armed)
    {
        connect_fault_armed = false;
        atomic_inc(&connect_fault_triggered);
        schedule_disconnect(connect_fault_delay_ms);
    }
    LOG_INF("Connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    atomic_inc(&physical_disconnects);
    LOG_INF("Disconnected (reason 0x%02x)", reason);
    k_spinlock_key_t key = k_spin_lock(&disconnect_lock);
    struct bt_conn *previous = NULL;
    if (active_connection == conn)
    {
        previous = active_connection;
        active_connection = NULL;
    }
    k_spin_unlock(&disconnect_lock, key);
    cancel_disconnect(conn);
    if (previous != NULL) bt_conn_unref(previous);
    state.notify_enabled = false;
    state.indicate_enabled = false;
    state.multi_notify_enabled = false;
    k_work_cancel_delayable(&burst_work);
    burst_remaining = 0;
    k_work_reschedule(&advertising_work, K_MSEC(100));
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
    ARG_UNUSED(conn);
    LOG_INF("MTU updated: TX %u RX %u", tx, rx);
}

static struct bt_gatt_cb gatt_callbacks = {
    .att_mtu_updated = mtu_updated,
};

int main(void)
{
    if (boot_magic != 0x48494c32) boot_count = 0;
    boot_magic = 0x48494c32;
    ++boot_count;
    k_work_init_delayable(&advertising_work, advertising_work_handler);
    k_work_init_delayable(&disconnect_work, disconnect_work_handler);
    k_work_init_delayable(&reboot_work, reboot_work_handler);
    k_work_init_delayable(&pressure_work, pressure_work_handler);
    k_work_init_delayable(&burst_work, burst_work_handler);
    k_work_init_delayable(&auxiliary_service_work,
                          auxiliary_service_work_handler);
    reset_state();

    int err = bt_enable(NULL);
    if (err != 0)
    {
        LOG_ERR("Bluetooth initialization failed (err %d)", err);
        return err;
    }
    bt_gatt_cb_register(&gatt_callbacks);

    err = start_advertising();
    if (err != 0)
    {
        LOG_ERR("Advertising failed (err %d)", err);
        return err;
    }

    LOG_INF("Universal BLE HIL peripheral ready (contract revision %d)",
            HIL_CONTRACT_REVISION);
    return 0;
}
