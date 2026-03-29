#ifndef ESP_CONFIG_H
#define ESP_CONFIG_H

#if !defined(FW_RELEASE) && !defined(FW_DIAG_UART0) && !defined(FW_DIAG_WIFI) && !defined(FW_DIAG_PROTOCOL)
  #define FW_RELEASE 1
#endif

#if defined(FW_DIAG_PROTOCOL)
  #define ENABLE_UART_CONSOLE        0
  #define ENABLE_AWG_AUTOSTREAM      1
  #define ENABLE_TRACE_RUNTIME       0
  #define ENABLE_WIFI_DIAG           0
  #define ENABLE_PROTOCOL_DIAG       1
  #define FW_VARIANT_NAME            "diag_protocol"
#elif defined(FW_DIAG_WIFI)
  #define ENABLE_UART_CONSOLE        1
  #define ENABLE_AWG_AUTOSTREAM      0
  #define ENABLE_TRACE_RUNTIME       1
  #define ENABLE_WIFI_DIAG           1
  #define ENABLE_PROTOCOL_DIAG       0
  #define FW_VARIANT_NAME            "diag_wifi"
#elif defined(FW_DIAG_UART0)
  #define ENABLE_UART_CONSOLE        1
  #define ENABLE_AWG_AUTOSTREAM      0
  #define ENABLE_TRACE_RUNTIME       1
  #define ENABLE_WIFI_DIAG           0
  #define ENABLE_PROTOCOL_DIAG       0
  #define FW_VARIANT_NAME            "diag_uart0"
#else
  #define ENABLE_UART_CONSOLE        0
  #define ENABLE_AWG_AUTOSTREAM      1
  #define ENABLE_TRACE_RUNTIME       0
  #define ENABLE_WIFI_DIAG           0
  #define ENABLE_PROTOCOL_DIAG       0
  #define FW_VARIANT_NAME            "release"
#endif

#ifndef ENABLE_AP_MAC_OVERRIDE
  #define ENABLE_AP_MAC_OVERRIDE     1
#endif

#define FW_BUILD_STRING __DATE__ " " __TIME__

/*
 * esp_config.h – Compile-time constants for espBode firmware
 * ============================================================
 * Network defaults are first-boot defaults only.
 * They can be overridden at runtime via the /config web page.
 */

/* ── First-boot WiFi / network defaults ─────────────────────────────────── */
#define DEF_SSID        "DSQ45"
#define DEF_PSK         "Fruit34Basky"
#define DEF_USE_DHCP    0       /* 0 = static IP (default) */
#define DEF_IP          10, 11, 13, 111
#define DEF_MASK        255, 255, 255, 0
#define DEF_GW          10, 11, 13, 1
#define DEF_DNS         10, 11, 13, 1

#define DEF_DEVICE_HOSTNAME       "espbode"
#define DEF_FRIENDLY_NAME         "espBode"
#define DEF_IDN_RESPONSE_NAME     "SDG1062X"
#define DEF_FALLBACK_AP_SSID      "ESP01"
#define DEF_FALLBACK_AP_PASSWORD  ""

/* Conservative fixed AWG identity used for Siglent Bode emulation. */
#define BODE_FIXED_ID_STRING      "IDN-SGLT-PRI SDG0000X"

/* ── AP fallback (when STA fails to connect) ────────────────────────────── */
#define AP_SSID         DEF_FALLBACK_AP_SSID
#define AP_PSK          DEF_FALLBACK_AP_PASSWORD
#define AP_IP_BYTES     192, 168, 0, 1
#define AP_MASK_BYTES   255, 255, 255, 0
#define AP_DHCP_START_BYTES 192, 168, 0, 10
#define AP_DHCP_END_BYTES   192, 168, 0, 50
#define STA_TIMEOUT_MS  30000    /* ms before switching to AP fallback */

/* ── Siglent AWG identity string ────────────────────────────────────────── */
#define IDN_RESPONSE_PREFIX   "IDN-SGLT-PRI "

/* ── RPC / VXI-11 port numbers ──────────────────────────────────────────── */
#define RPC_PORT        111
#define VXI_PORT_A      9009
#define VXI_PORT_B      9010
#define VXI_STABLE_PORT VXI_PORT_A

/* ── RPC / XDR constants ────────────────────────────────────────────────── */
#define PORTMAP_PROG    0x000186A0UL
#define VXI11_CORE_PROG 0x000607AFUL
#define PORTMAP_VERS    2U
#define VXI11_CORE_VERS 1U

#define PORTMAP_GETPORT     3U
#define VXI11_CREATE_LINK   10U
#define VXI11_DEVICE_WRITE  11U
#define VXI11_DEVICE_READ   12U
#define VXI11_DESTROY_LINK  23U

#define RPC_CALL            0U
#define RPC_REPLY           1U
#define MSG_ACCEPTED        0U
#define SUCCESS_STAT        0U
#define RPC_SINGLE_FRAG     0x80000000UL

/* ── Buffer sizes ───────────────────────────────────────────────────────── */
#define RX_BUF_SIZE     288     /* max incoming RPC packet (header + SCPI cmd) */
#define TX_BUF_SIZE     128     /* max outgoing RPC packet                     */
#define CMD_BUF_SIZE    256     /* SCPI command buffer                         */
#define RESP_BUF_SIZE   128     /* SCPI query response buffer                  */

/* ── FY6900 serial baud rate ────────────────────────────────────────────── */
#define AWG_BAUD_RATE   115200
#define AWG_SERIAL_TIMEOUT_MS  1200
#define AUTO_OUTPUT_OFF_TIMEOUT_MS 30000UL

/* ── Session idle timeout ───────────────────────────────────────────────── */
#define VXI_SESSION_TIMEOUT_MS  8000

/* ── Debug output ───────────────────────────────────────────────────────── */
/*
 * RELEASE BUILD (default): no debug output whatsoever.
 *   Serial is exclusively used for FY6900 AWG communication.
 *
 * DEBUG BUILD: add -D DEBUG_BUILD to build_flags in platformio.ini.
 *   Debug text goes to Serial1 (GPIO2, TX-only).
 *   Connect a 3.3 V UART adapter to GPIO2 at 115200 baud to read it.
 *   Never enable DEBUG_BUILD when deploying to avoid Serial1 GPIO conflicts.
 */
#ifdef DEBUG_BUILD
  #define DBG_INIT()    do { Serial1.begin(115200); } while(0)
  #define DBG(x)        Serial1.println(x)
  #define DBGF(...)     Serial1.printf(__VA_ARGS__)
#else
  #define DBG_INIT()
  #define DBG(x)
  #define DBGF(...)
#endif

#endif /* ESP_CONFIG_H */
