// Bluetooth LE "Nordic UART Service" (from the EyeBuddy firmware): the website writes text lines, the board notifies lines back.
#pragma once
#include <stddef.h>

void ble_begin(const char* name);
size_t ble_read(char* buf, size_t max);   // bytes received since last call (non-blocking)
void ble_send_line(const char* line);     // adds '\n'; no-op when nobody is connected
bool ble_connected();
