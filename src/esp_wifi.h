#ifndef ESP_WIFI_H
#define ESP_WIFI_H

#include <ESP8266WiFi.h>

/*
 * esp_wifi.h – WiFi initialisation
 * =================================
 * Tries STA connection for STA_TIMEOUT_MS.
 * On failure falls back to AP mode so the user can reach the config page.
 */

typedef enum {
	WIFI_FAIL_NONE = 0,
	WIFI_FAIL_TIMEOUT,
	WIFI_FAIL_SSID_NOT_FOUND,
	WIFI_FAIL_AUTH_FAILED,
	WIFI_FAIL_CONNECT_FAILED,
	WIFI_FAIL_DHCP_FAILED,
	WIFI_FAIL_STATIC_IP_FAILED,
	WIFI_FAIL_INVALID_CONFIG
} WifiFailReason;

/* Returns true  = connected as STA
 *         false = AP fallback started */
bool wifi_setup();

/* Returns true when running as STA (false = AP mode) */
bool wifi_is_sta();

bool wifi_is_connected();
WifiFailReason wifi_last_fail_reason();
const char *wifi_last_fail_reason_text();
const char *wifi_mode_text();
IPAddress wifi_current_ip();
bool wifi_start_fallback_ap_for_diag(void);

#endif /* ESP_WIFI_H */
