#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Starts the Wi-Fi station and keeps it connected: after every disconnection it tries again with exponential backoff
 * and jitter, forever. Needs the default event loop and esp_netif to be initialized.
 */
void wifi_start(const char *ssid, const char *password);

/**
 * Waits until the station has an IP address. Returns false on timeout.
 */
bool wifi_wait_connected(uint32_t timeout_ms);
