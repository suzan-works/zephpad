#pragma once
#include <stdbool.h>
#include <stdint.h>

bool ble_hid_subscribed(void);
void ble_hid_disconnected(void);
int ble_hid_send(uint8_t modifiers, uint8_t key);
