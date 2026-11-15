// Bluetooth LE "Nordic UART Service" (same as the EyeBuddy firmware): the web app writes text lines, the board notifies lines back.
// One laptop at a time: while connected the board stops advertising (others can't even see it).
#pragma once
#include <stddef.h>
#include <stdint.h>

void ble_begin(const char* name);
size_t ble_read(char* buf, size_t max);   // bytes received since last call (non-blocking)
void ble_send_line(const char* line);     // adds '\n', sent in 20-byte notifications; no-op when nobody is connected
bool ble_connected();
uint32_t ble_drops();                     // counts disconnects; changes => the laptop went away
void ble_disconnect();                    // kick the connected laptop (it may reconnect, then must pair again)
