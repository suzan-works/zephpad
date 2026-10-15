#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>
#include <errno.h>
#include "app_config.h"
#include "power_policy.h"
#include "ble_hid.h"

/* ピン番号はapp.overlayに集約。配列の順番はSW1～SW4です。 */
static const struct gpio_dt_spec keys[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(zp_sw1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(zp_sw2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(zp_sw3), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(zp_sw4), gpios),
};
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(zp_status), gpios);
static atomic_t linked, encrypted, accepting, dictation_gate, overflow;
static atomic_t shutting_down, activity_generation;

static void mark_activity(void)
{
    /* コールバックは操作回数だけ更新。時刻の計算はmainだけで行います。 */
    atomic_inc(&activity_generation);
}
/* 入力コールバックからmainへ、操作を順番に渡すキューです。 */
K_MSGQ_DEFINE(events, sizeof(struct input_event), 32, 4);

static bool ready(void)
{
    /* BLE接続だけでなく、暗号化とPC側のHID通知登録も必要です。 */
    return atomic_get(&linked) && atomic_get(&encrypted) && ble_hid_subscribed();
}

/* mainだけから呼びます。通常のキー操作では点灯時間を延長しません。
 * readyがfalseに戻ると待機点滅へ移り、再接続時は再び3秒点灯します。 */
static void update_status_led(bool can_send, int64_t now_ms)
{
    static bool was_ready;
    static int64_t ready_since_ms;
    bool on;

    if (can_send) {
        if (!was_ready) {
            ready_since_ms = now_ms;
        }
        on = now_ms - ready_since_ms < ZEPHPAD_CONNECTED_LED_MS;
    } else {
        on = (now_ms / ZEPHPAD_WAIT_LED_HALF_PERIOD_MS) % 2 == 0;
    }
    was_ready = can_send;
    gpio_pin_set_dt(&led, on);
}

static void input_cb(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);
    if (evt->type == INPUT_EV_KEY || evt->type == INPUT_EV_REL) {
        mark_activity();
    }
    if (!atomic_get(&accepting)) { return; }
    bool voice = evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_F3;
    if (voice) {
        atomic_set(&dictation_gate, !!evt->value);
    } else if (atomic_get(&dictation_gate)) {
        return; /* Discard; never replay other input after dictation. */
    }
    if (k_msgq_put(&events, evt, K_NO_WAIT)) { atomic_set(&overflow, 1); }
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xc1, 0x03),
};
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME)-1),
};
static void advertise(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(adv_work, advertise);
static void advertise(struct k_work *work)
{
    ARG_UNUSED(work);
    if (atomic_get(&linked) || atomic_get(&shutting_down)) { return; }
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err && err != -EALREADY && !atomic_get(&shutting_down)) {
        k_work_reschedule(&adv_work, K_MSEC(500));
    }
}
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        if (!atomic_get(&shutting_down)) { k_work_reschedule(&adv_work, K_MSEC(500)); }
        return;
    }
    mark_activity();
    atomic_set(&linked, 1);
    atomic_clear(&encrypted);
    if (atomic_get(&shutting_down)) { bt_conn_disconnect(conn, 0x13); return; }
    int ret = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (ret) { bt_conn_disconnect(conn, 0x13); }
}
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn); ARG_UNUSED(reason);
    atomic_clear(&accepting);
    atomic_clear(&linked);
    atomic_clear(&encrypted);
    ble_hid_disconnected();
    mark_activity();
    if (!atomic_get(&shutting_down)) { k_work_reschedule(&adv_work, K_MSEC(300)); }
}
static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err)
{
    atomic_set(&encrypted, !err && level >= BT_SECURITY_L2);
    mark_activity();
    if (err) { bt_conn_disconnect(conn, 0x13); }
}
BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};

static void fatal(void)
{
    for (;;) {
        gpio_pin_toggle_dt(&led);
        k_sleep(K_MSEC(80));
    }
}
static bool all_released(void)
{
    for (size_t i=0; i<ARRAY_SIZE(keys); i++) {
        if (gpio_pin_get_dt(&keys[i]) != 0) { return false; }
    }
    return true;
}
static int send_retry(uint8_t mod, uint8_t key)
{
    for (int i=0; i<20 && ready(); i++) {
        int err = ble_hid_send(mod, key);
        if (!err) { return 0; }
        k_sleep(K_MSEC(10));
    }
    return -EIO;
}
static int tap(uint8_t mod, uint8_t key)
{
    int err = send_retry(mod, key);
    if (!err) { k_sleep(K_MSEC(ZEPHPAD_TAP_MS)); }
    int release_err = send_retry(0, 0);
    k_sleep(K_MSEC(ZEPHPAD_TAP_MS));
    return err ? err : release_err;
}
static void input_reset(void)
{
    atomic_clear(&accepting);
    k_msgq_purge(&events);
    atomic_clear(&dictation_gate);
    atomic_clear(&overflow);
}
/* Errors during shutdown remain awake and blink N times, then pause.
 * A failed shutdown must not be mistaken for successful System OFF. */
static void power_error(unsigned int count)
{
    for (;;) {
        for (unsigned int i = 0; i < count; i++) {
            gpio_pin_set_dt(&led, 1); k_sleep(K_MSEC(150));
            gpio_pin_set_dt(&led, 0); k_sleep(K_MSEC(150));
        }
        k_sleep(K_MSEC(1200));
    }
}

static void disconnect_one(struct bt_conn *conn, void *data)
{
    ARG_UNUSED(data);
    bt_conn_disconnect(conn, 0x13); /* Remote User Terminated Connection */
}

static bool suspend_ok(const struct device *dev)
{
    int err = pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
    return err == 0 || err == -EALREADY;
}

/* Returns only when shutdown was cancelled; leave queued input intact. */
static void enter_system_off(const struct zp_power_sample *sample)
{
    if (!all_released()) { return; }
    k_sleep(K_MSEC(50));
    if (!all_released() || k_msgq_num_used_get(&events) != 0 ||
        sample->generation != (uint32_t)atomic_get(&activity_generation) ||
        sample->linked != (atomic_get(&linked) != 0) ||
        sample->ready != ready()) {
        return;
    }
    atomic_set(&shutting_down, 1);
    input_reset();

    if (ready()) {
        (void)send_retry(0, 0);
        k_sleep(K_MSEC(40));
    }
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&adv_work, &sync);
    (void)bt_le_adv_stop();
    bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_one, NULL);
    int64_t deadline = k_uptime_get() + 1500;
    while (atomic_get(&linked) && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(10));
    }
    if (atomic_get(&linked)) { power_error(1); }

    /* Stop radio/host without erasing persistent bonds. */
    int err = bt_disable();
    if (err && err != -EALREADY) { power_error(2); }
    if (!suspend_ok(DEVICE_DT_GET(DT_NODELABEL(zephpad_encoder)))) { power_error(3); }
    if (!suspend_ok(DEVICE_DT_GET(DT_NODELABEL(zephpad_keys)))) { power_error(4); }

    /* QDEC suspend may preserve DT pullups. Remove them explicitly: a
     * stationary encoder contact must not draw current through a pullup. */
    const struct gpio_dt_spec encoder_pins[] = {
        GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(zephpad_encoder), gpios, 0),
        GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(zephpad_encoder), gpios, 1),
    };
    for (size_t i = 0; i < ARRAY_SIZE(encoder_pins); i++) {
        const struct gpio_dt_spec *p = &encoder_pins[i];
        if (gpio_pin_interrupt_configure_dt(p, GPIO_INT_DISABLE) ||
            gpio_pin_configure(p->port, p->pin, GPIO_DISCONNECTED)) { power_error(5); }
    }
    for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
        if (gpio_pin_interrupt_configure_dt(&keys[i], GPIO_INT_DISABLE) ||
            gpio_pin_configure(keys[i].port, keys[i].pin, GPIO_DISCONNECTED)) {
            power_error(5);
        }
    }

    /* XIAO's external QSPI flash has a deep-power-down-capable driver.
     * The standard board stores BLE settings in INTERNAL storage_partition. */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(p25q16h), okay) && defined(CONFIG_NORDIC_QSPI_NOR)
    const struct device *ext_flash = DEVICE_DT_GET(DT_NODELABEL(p25q16h));
    if (!device_is_ready(ext_flash) || !suspend_ok(ext_flash)) { power_error(6); }
#endif

    gpio_pin_set_dt(&led, 0);
    /* Only SW4 (D3/P0.29, active low with pullup) is armed as GPIO wake. */
    if (gpio_pin_configure_dt(&keys[3], GPIO_INPUT) ||
        gpio_pin_interrupt_configure_dt(&keys[3], GPIO_INT_LEVEL_ACTIVE)) {
        power_error(7);
    }
    /* A new SW4 press here can cause immediate reset/wake, which is correct. */
    sys_poweroff();
}

static int handle_event(const struct input_event *e)
{
    /* HID modifiers: left Ctrl=1, Shift=2, Alt=4; Meh=7. */
    if (e->type == INPUT_EV_KEY) {
        if (e->code == INPUT_KEY_F3) {
            return e->value ? send_retry(0x07, 0x07) : send_retry(0, 0);
        }
        if (!e->value) { return 0; }
        switch (e->code) {
        case INPUT_KEY_F1: return tap(0, 0x29);    /* Esc */
        case INPUT_KEY_F2: return tap(1, 0x11);    /* Ctrl+N */
        case INPUT_KEY_F4: return tap(0, 0x28);    /* Enter */
        default: break;
        }
    } else if (e->type == INPUT_EV_REL && e->code == INPUT_REL_WHEEL) {
        int count = CLAMP(e->value * ZEPHPAD_ENCODER_SIGN, -16, 16);
        uint8_t key = count > 0 ? 0x52 : 0x51; /* up / down */
        if (count < 0) { count = -count; }
        for (int i=0; i<count; i++) {
            if (atomic_get(&dictation_gate)) { break; }
            int err = tap(7, key);
            if (err) { return err; }
        }
    }
    return 0;
}

int main(void)
{
    if (!gpio_is_ready_dt(&led)) { return 0; }
    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE)) { return 0; }
    if (!device_is_ready(DEVICE_DT_GET(DT_NODELABEL(zephpad_keys))) ||
        !device_is_ready(DEVICE_DT_GET(DT_NODELABEL(zephpad_encoder)))) { fatal(); }

    /* Explicit local recovery: SW1+SW2 held through reset for 2 seconds. */
    bool erase_bonds = gpio_pin_get_dt(&keys[0]) == 1 && gpio_pin_get_dt(&keys[1]) == 1;
    if (erase_bonds) {
        for (int i=0; i<100; i++) {
            if (gpio_pin_get_dt(&keys[0]) != 1 || gpio_pin_get_dt(&keys[1]) != 1) {
                erase_bonds = false; break;
            }
            gpio_pin_toggle_dt(&led);
            k_sleep(K_MSEC(20));
        }
    }
    if (bt_enable(NULL)) { fatal(); }
    if (settings_load()) { fatal(); }
    if (erase_bonds && bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY)) { fatal(); }
    k_work_reschedule(&adv_work, K_NO_WAIT);

    struct zp_power_policy power = {0};
    bool armed = false;
    int64_t released_since = 0;
    for (;;) {
        bool can_send = ready();
        update_status_led(can_send, k_uptime_get());
        if (!can_send || atomic_get(&overflow)) {
            input_reset();
            armed = false;
            released_since = 0;
        }
        if (!armed && can_send) {
            /* Startup/reconnect: all keys must be released for 50 ms. */
            if (!all_released()) {
                released_since = 0;
            } else if (!released_since) {
                released_since = k_uptime_get();
            } else if (k_uptime_get() - released_since >= 50 && !send_retry(0,0)) {
                input_reset();
                armed = true;
                atomic_set(&accepting, 1);
            }
        }
        struct input_event evt;
        if (!k_msgq_get(&events, &evt, K_MSEC(20)) && armed && ready()) {
            if (handle_event(&evt)) {
                input_reset();
                armed = false;
                released_since = 0;
            }
        }

        /* Only main owns the clock and deadline. Callback activity is a
         * generation counter, so an asynchronous update cannot underflow
         * an unsigned elapsed-time subtraction and request immediate OFF. */
        struct zp_power_sample sample = {
            .generation = (uint32_t)atomic_get(&activity_generation),
            .linked = atomic_get(&linked) != 0,
            .ready = ready(),
            .busy = !all_released() || k_msgq_num_used_get(&events) != 0 ||
                    (can_send && !armed) || atomic_get(&overflow),
            .now_ms = k_uptime_get(),
        };
        if (zp_power_due(&power, &sample, ZEPHPAD_AUTO_OFF,
                         ZEPHPAD_IDLE_OFF_MS, ZEPHPAD_OFFLINE_OFF_MS)) {
            enter_system_off(&sample);
            /* New activity cancelled OFF; retain the event and start over. */
            zp_power_postpone(&power, k_uptime_get());
        }
    }
}
