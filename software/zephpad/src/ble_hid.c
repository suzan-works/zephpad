/* Report-mode BLE keyboard. No Boot Protocol or Consumer Control. */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>
#include "ble_hid.h"

static atomic_t subscribed;
static uint8_t current_report[8];
static uint8_t keyboard_leds;
static uint8_t control_point;
static const uint8_t info[] = {0x11, 0x01, 0x00, 0x02};
static const uint8_t input_ref[] = {1, 1};
static const uint8_t output_ref[] = {1, 2};
static const uint8_t report_map[] = {
    0x05,0x01, 0x09,0x06, 0xa1,0x01, 0x85,0x01,
    0x05,0x07, 0x19,0xe0, 0x29,0xe7, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x75,0x08, 0x95,0x01, 0x81,0x01,
    0x05,0x08, 0x19,0x01, 0x29,0x05, 0x75,0x01, 0x95,0x05,
    0x91,0x02, 0x75,0x03, 0x95,0x01, 0x91,0x01,
    0x05,0x07, 0x19,0x00, 0x29,0x65, 0x15,0x00, 0x25,0x65,
    0x75,0x08, 0x95,0x06, 0x81,0x00, 0xc0
};

#define READ_FN(name, data) \
static ssize_t name(struct bt_conn *c, const struct bt_gatt_attr *a, \
                    void *b, uint16_t n, uint16_t o) \
{ return bt_gatt_attr_read(c, a, b, n, o, data, sizeof(data)); }
READ_FN(read_info, info)
READ_FN(read_map, report_map)
READ_FN(read_input_ref, input_ref)
READ_FN(read_output_ref, output_ref)
READ_FN(read_input, current_report)

static ssize_t read_output(struct bt_conn *c, const struct bt_gatt_attr *a,
                           void *b, uint16_t n, uint16_t o)
{
    return bt_gatt_attr_read(c, a, b, n, o, &keyboard_leds, 1);
}
static ssize_t write_byte(struct bt_conn *c, const struct bt_gatt_attr *a,
                          const void *b, uint16_t n, uint16_t o, uint8_t f)
{
    ARG_UNUSED(c); ARG_UNUSED(f);
    if (o != 0) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); }
    if (n != 1) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN); }
    *(uint8_t *)a->user_data = *(const uint8_t *)b;
    return n;
}
static void ccc_changed(const struct bt_gatt_attr *a, uint16_t value)
{
    ARG_UNUSED(a);
    atomic_set(&subscribed, value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(keyboard_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),                         /* 0 */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ, read_info, NULL, NULL),                 /* 1,2 */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ_ENCRYPT, read_map, NULL, NULL),           /* 3,4 */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ_ENCRYPT, read_input, NULL, NULL),         /* 5,6 */
    BT_GATT_CCC(ccc_changed,
        BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),    /* 7 */
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
        BT_GATT_PERM_READ_ENCRYPT, read_input_ref, NULL, NULL),     /* 8 */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
        read_output, write_byte, &keyboard_leds),                  /* 9,10 */
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
        BT_GATT_PERM_READ_ENCRYPT, read_output_ref, NULL, NULL),    /* 11 */
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT,
        BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE_ENCRYPT,
        NULL, write_byte, &control_point)                         /* 12,13 */
);

bool ble_hid_subscribed(void) { return atomic_get(&subscribed) != 0; }
void ble_hid_disconnected(void) { atomic_clear(&subscribed); }
int ble_hid_send(uint8_t modifiers, uint8_t key)
{
    uint8_t report[8] = {modifiers, 0, key, 0, 0, 0, 0, 0};
    if (!ble_hid_subscribed()) { return -ENOTCONN; }
    int err = bt_gatt_notify(NULL, &keyboard_svc.attrs[6], report, sizeof(report));
    if (!err) { memcpy(current_report, report, sizeof(report)); }
    return err;
}
