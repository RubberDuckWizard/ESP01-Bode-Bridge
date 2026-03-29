/*
 * espBode.ino – Main entry point
 * =====================================================================
 * ESP-01 / ESP8266EX firmware bridge between a Siglent SDS800X-HD
 * oscilloscope and a Feeltech FY6900 AWG (later firmware, Hz format).
 *
 * Arduino / PlatformIO entry point – deliberately thin.
 * All logic lives in the helper modules.
 *
 * Hardware:
 *   ESP-01 / W25Q80DV (1 MB flash, 26 MHz crystal)
 *   Serial (GPIO1-TX / GPIO3-RX) → FY6900 TTL serial
 *   WiFi → Siglent oscilloscope LAN
 *
 * Build:  pio run                              (PlatformIO)
 * Flash:  pio run -t upload                    (or see README / FLASH.md)
 * Output: .pio/build/esp01_1m/firmware.bin
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "esp_config.h"
#include "esp_persist.h"
#include "esp_wifi.h"
#include "esp_fy6900.h"
#include "esp_diag_console.h"
#include "esp_parser.h"
#include "esp_network.h"
#include "esp_webconfig.h"

/* ── setup ──────────────────────────────────────────────────────────────── */
void setup()
{
    diag_begin_early();

    /* Debug UART on Serial1 (GPIO2, TX only) – release builds: NOP */
    DBG_INIT();
    DBG("\n\n=== espBode starting ===");

    diag_tracef("\r\nvariant=%s build=%s\r\n", FW_VARIANT_NAME, FW_BUILD_STRING);

    /* 1. Load persistent config (or install first-boot defaults) */
    config_init();

    diag_tracef("stored_config_valid=%s ssid=%s dhcp=%s heap=%lu\r\n",
        config_store_was_valid() ? "yes" : "no",
        g_config.ssid,
        g_config.use_dhcp ? "on" : "off",
        (unsigned long)ESP.getFreeHeap());
    if (!g_config.use_dhcp) {
        diag_tracef("static_ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\r\n",
            g_config.ip[0], g_config.ip[1], g_config.ip[2], g_config.ip[3],
            g_config.mask[0], g_config.mask[1], g_config.mask[2], g_config.mask[3],
            g_config.gw[0], g_config.gw[1], g_config.gw[2], g_config.gw[3],
            g_config.dns[0], g_config.dns[1], g_config.dns[2], g_config.dns[3]);
    }

    /* 2. Initialise FY6900 AWG on Serial (UART0) */
#if !ENABLE_WIFI_DIAG
    fy_init();
#else
    diag_tracef("fy_init skipped for wifi diagnostics\r\n");
#endif

    /* 3. Connect to WiFi (STA or AP fallback) */
    wifi_setup();

    /* 4. Start VXI-11 / portmapper network services */
    net_begin();

    /* 5. Start web configuration server on port 80 */
    webconfig_begin();

    /* 6. Optional diagnostic UART0 console */
    diag_setup();

    DBGF("Ready – VXI on port %u\n", (unsigned)9009);
}

/* ── loop ───────────────────────────────────────────────────────────────── */
void loop()
{
    /* Poll all non-blocking services */
    net_poll();        /* UDP/TCP portmapper + VXI-11 session */
    webconfig_poll();  /* HTTP config/reset page */
    diag_poll();       /* optional UART0 diagnostic console */
    yield();           /* feed ESP8266 watchdog / WiFi stack */
}
