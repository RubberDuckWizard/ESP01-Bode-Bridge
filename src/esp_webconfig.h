#ifndef ESP_WEBCONFIG_H
#define ESP_WEBCONFIG_H

/*
 * esp_webconfig.h – Minimal port-80 configuration web UI
 * =======================================================
 * Available in both STA mode (change settings after deployment) and
 * AP fallback mode (initial or recovery configuration).
 *
 * Routes:
 *   GET  /        → redirect to /config
 *   GET  /config  → HTML form with current settings
 *   POST /config  → validate, save, reboot
 *   GET  /reset   → confirm factory reset (requires ?confirm=yes)
 *   GET  /diag    → diagnostic page (raw FY command test, DEBUG_BUILD only)
 */

void webconfig_begin(void);
void webconfig_poll(void);

#endif /* ESP_WEBCONFIG_H */
