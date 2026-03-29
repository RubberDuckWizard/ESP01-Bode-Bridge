#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_config.h"
#include "esp_diag_console.h"
#include "esp_fy6900.h"
#include "esp_network.h"
#include "esp_persist.h"
#include "esp_webconfig.h"
#include "esp_wifi.h"

#if ENABLE_UART_CONSOLE

static bool s_trace_enabled = ENABLE_WIFI_DIAG ? true : false;
static char s_line[128];
static uint8_t s_line_len = 0;

void diag_begin_early(void)
{
    Serial.begin(115200);
    Serial.setTimeout(200);
    delay(10);
}

static void diag_print_prompt(void)
{
    Serial.print("diag> ");
}

static void diag_printf(const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(buf);
}

bool diag_trace_enabled(void)
{
    return s_trace_enabled;
}

void diag_trace_set(bool enabled)
{
    s_trace_enabled = enabled;
}

void diag_tracef(const char *fmt, ...)
{
    if (!s_trace_enabled) return;
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(buf);
}

static const char *wifi_mode_name(void)
{
    return wifi_mode_text();
}

static void print_ip4(const uint8_t *ip4)
{
    diag_printf("%u.%u.%u.%u", ip4[0], ip4[1], ip4[2], ip4[3]);
}

static bool parse_ip4(const char *text, uint8_t *out)
{
    unsigned a, b, c, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = (uint8_t)a;
    out[1] = (uint8_t)b;
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)d;
    return true;
}

static void cmd_help(void)
{
    diag_printf("help\r\n");
    diag_printf("status\r\n");
    diag_printf("wifi | wifi scan | wifi connect\r\n");
#if ENABLE_WIFI_DIAG
    diag_printf("cfg show\r\n");
    diag_printf("reboot\r\n");
    diag_printf("ap start | sta retry\r\n");
#else
    diag_printf("cfg show | cfg reset | cfg dhcp on|off | cfg ip <ip> <mask> <gw> <dns>\r\n");
    diag_printf("reboot\r\n");
    diag_printf("rpc | vxi | sessions\r\n");
    diag_printf("awg enable | awg disable | awg raw <command> | awg poll | awg baud <value>\r\n");
    diag_printf("trace on | trace off\r\n");
#endif
}

static void cmd_status(void)
{
#if !ENABLE_WIFI_DIAG
    const NetStats *stats = net_get_stats();
#endif
    diag_printf("firmware=%s build=%s\r\n", FW_VARIANT_NAME, FW_BUILD_STRING);
    diag_printf("wifi=%s connected=%s ip=%s\r\n",
        wifi_mode_name(),
        (WiFi.status() == WL_CONNECTED) ? "yes" : "no",
        wifi_current_ip().toString().c_str());
    diag_printf("wifi_status=%d fail=%s\r\n",
        (int)WiFi.status(),
        wifi_last_fail_reason_text());
    diag_printf("cfg stored_valid=%s current_crc=%s ssid=%s dhcp=%s\r\n",
        config_store_was_valid() ? "yes" : "no",
        config_current_is_valid() ? "yes" : "no",
        g_config.ssid,
        g_config.use_dhcp ? "on" : "off");
#if ENABLE_WIFI_DIAG
    diag_printf("heap=%lu ap_mac=%s\r\n",
        (unsigned long)ESP.getFreeHeap(),
        WiFi.softAPmacAddress().c_str());
#else
    diag_printf("awg enabled=%s baud=%lu trace=%s\r\n",
        fy_is_enabled() ? "yes" : "no",
        (unsigned long)fy_get_baud(),
        s_trace_enabled ? "on" : "off");
    diag_printf("vxi port=%u session=%s last=%s\r\n",
        stats->current_vxi_port,
        stats->session_active ? "active" : "idle",
        stats->last_event);
    #endif
}

static void cmd_wifi(void)
{
    diag_printf("mode=%s status=%d fail=%s rssi=%d mac=%s\r\n",
        wifi_mode_name(),
        (int)WiFi.status(),
        wifi_last_fail_reason_text(),
        (int)WiFi.RSSI(),
        WiFi.macAddress().c_str());

    if (wifi_is_sta()) {
        diag_printf("ip=%s gw=%s dns=%s subnet=%s\r\n",
            WiFi.localIP().toString().c_str(),
            WiFi.gatewayIP().toString().c_str(),
            WiFi.dnsIP().toString().c_str(),
            WiFi.subnetMask().toString().c_str());
    } else {
        diag_printf("ap_ip=%s clients=%d\r\n",
            WiFi.softAPIP().toString().c_str(),
            WiFi.softAPgetStationNum());
    }
}

static void cmd_wifi_scan(void)
{
    int count = WiFi.scanNetworks();
    if (count < 0) {
        diag_printf("scan failed\r\n");
        return;
    }
    diag_printf("scan count=%d\r\n", count);
    for (int i = 0; i < count; i++) {
        diag_printf("%2d: %s rssi=%d enc=%d\r\n",
            i + 1,
            WiFi.SSID(i).c_str(),
            WiFi.RSSI(i),
            (int)WiFi.encryptionType(i));
    }
    WiFi.scanDelete();
}

static void cmd_wifi_connect(void)
{
    bool sta = wifi_setup();
    diag_printf("wifi connect result=%s fail=%s ip=%s\r\n",
        sta ? "sta" : "ap_fallback",
        wifi_last_fail_reason_text(),
        wifi_current_ip().toString().c_str());
}

static void cmd_ap_start(void)
{
    wifi_start_fallback_ap_for_diag();
    diag_printf("ap start mode=%d ip=%s mac=%s fail=%s\r\n",
        (int)WiFi.getMode(),
        WiFi.softAPIP().toString().c_str(),
        WiFi.softAPmacAddress().c_str(),
        wifi_last_fail_reason_text());
}

static void cmd_cfg_show(void)
{
    diag_printf("stored_valid=%s current_crc=%s version=%u\r\n",
        config_store_was_valid() ? "yes" : "no",
        config_current_is_valid() ? "yes" : "no",
        g_config.version);
    diag_printf("ssid=%s psk=%s dhcp=%s\r\n",
        g_config.ssid,
        g_config.psk,
        g_config.use_dhcp ? "on" : "off");
    diag_printf("ip="); print_ip4(g_config.ip);
    diag_printf(" mask="); print_ip4(g_config.mask);
    diag_printf(" gw="); print_ip4(g_config.gw);
    diag_printf(" dns="); print_ip4(g_config.dns);
    diag_printf("\r\n");
}

static void cmd_cfg_reset(void)
{
    diag_printf("restoring defaults and rebooting\r\n");
    config_reset_defaults();
    config_save();
    delay(200);
    ESP.restart();
}

static void cmd_cfg_dhcp(bool enabled)
{
    g_config.use_dhcp = enabled ? 1 : 0;
    config_save();
    diag_printf("dhcp=%s saved\r\n", enabled ? "on" : "off");
}

static void cmd_cfg_ip(char *args)
{
    char ip[20], mask[20], gw[20], dns[20];
    if (sscanf(args, "%19s %19s %19s %19s", ip, mask, gw, dns) != 4) {
        diag_printf("usage: cfg ip <ip> <mask> <gw> <dns>\r\n");
        return;
    }
    if (!parse_ip4(ip, g_config.ip) || !parse_ip4(mask, g_config.mask)
            || !parse_ip4(gw, g_config.gw) || !parse_ip4(dns, g_config.dns)) {
        diag_printf("invalid ip tuple\r\n");
        return;
    }
    config_save();
    diag_printf("static ip tuple saved\r\n");
}

static void cmd_rpc(void)
{
    const NetStats *stats = net_get_stats();
    diag_printf("udp_getport=%lu tcp_getport=%lu zero_reply=%lu\r\n",
        (unsigned long)stats->udp_getport_count,
        (unsigned long)stats->tcp_getport_count,
        (unsigned long)stats->getport_zero_reply_count);
    diag_printf("create=%lu write=%lu read=%lu destroy=%lu last=%s\r\n",
        (unsigned long)stats->create_link_count,
        (unsigned long)stats->device_write_count,
        (unsigned long)stats->device_read_count,
        (unsigned long)stats->destroy_link_count,
        stats->last_event);
}

static void cmd_vxi(void)
{
    const NetStats *stats = net_get_stats();
    diag_printf("current_vxi_port=%u active=%s client=%s:%u\r\n",
        stats->current_vxi_port,
        stats->session_active ? "yes" : "no",
        stats->active_client_ip.toString().c_str(),
        stats->active_client_port);
    diag_printf("last_write=%lu last_read=%lu last=%s\r\n",
        (unsigned long)stats->last_write_len,
        (unsigned long)stats->last_read_len,
        stats->last_event);
}

static void cmd_sessions(void)
{
    const NetStats *stats = net_get_stats();
    diag_printf("accepted=%lu ended=%lu dropped=%lu active=%s\r\n",
        (unsigned long)stats->session_accept_count,
        (unsigned long)stats->session_end_count,
        (unsigned long)stats->session_drop_count,
        stats->session_active ? "yes" : "no");
    diag_printf("client=%s:%u current_vxi_port=%u\r\n",
        stats->active_client_ip.toString().c_str(),
        stats->active_client_port,
        stats->current_vxi_port);
}

static void cmd_awg_raw(const char *cmd)
{
    char reply[96];
    int n;
    if (!fy_is_enabled()) {
        diag_printf("awg disabled\r\n");
        return;
    }
    diag_printf("raw tx: %s\r\n", cmd);
    n = fy_raw_cmd(cmd, reply, sizeof(reply));
    if (n < 0) {
        diag_printf("raw rejected\r\n");
        return;
    }
    diag_printf("raw rx (%d): %s\r\n", n, reply);
}

static void cmd_awg_poll(void)
{
    char buf[96];
    int n = fy_read_available(buf, sizeof(buf));
    diag_printf("awg poll bytes=%d\r\n", n);
    if (n > 0) diag_printf("%s\r\n", buf);
}

static void process_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') {
        diag_print_prompt();
        return;
    }

    if (strcmp(line, "help") == 0) cmd_help();
    else if (strcmp(line, "status") == 0) cmd_status();
    else if (strcmp(line, "wifi") == 0) cmd_wifi();
    else if (strcmp(line, "wifi scan") == 0) cmd_wifi_scan();
    else if (strcmp(line, "wifi connect") == 0) cmd_wifi_connect();
    else if (strcmp(line, "cfg show") == 0) cmd_cfg_show();
#if ENABLE_WIFI_DIAG
    else if (strcmp(line, "ap start") == 0) cmd_ap_start();
    else if (strcmp(line, "sta retry") == 0) cmd_wifi_connect();
#else
    else if (strcmp(line, "cfg reset") == 0) cmd_cfg_reset();
    else if (strcmp(line, "cfg dhcp on") == 0) cmd_cfg_dhcp(true);
    else if (strcmp(line, "cfg dhcp off") == 0) cmd_cfg_dhcp(false);
    else if (strncmp(line, "cfg ip ", 7) == 0) cmd_cfg_ip(line + 7);
    else if (strcmp(line, "reboot") == 0) { diag_printf("rebooting\r\n"); delay(100); ESP.restart(); }
    else if (strcmp(line, "rpc") == 0) cmd_rpc();
    else if (strcmp(line, "vxi") == 0) cmd_vxi();
    else if (strcmp(line, "sessions") == 0) cmd_sessions();
    else if (strcmp(line, "awg enable") == 0) { fy_set_enabled(true); diag_printf("awg enabled\r\n"); }
    else if (strcmp(line, "awg disable") == 0) { fy_set_enabled(false); diag_printf("awg disabled\r\n"); }
    else if (strncmp(line, "awg raw ", 8) == 0) cmd_awg_raw(line + 8);
    else if (strcmp(line, "awg poll") == 0) cmd_awg_poll();
    else if (strncmp(line, "awg baud ", 9) == 0) {
        unsigned long baud = strtoul(line + 9, NULL, 10);
        if (!fy_is_supported_baud((uint32_t)baud)) {
            diag_printf("unsupported baud (9600,19200,38400,57600,115200)\r\n");
        } else {
            diag_printf("changing uart0 baud to %lu\r\n", baud);
            Serial.flush();
            fy_set_baud((uint32_t)baud);
            delay(50);
            Serial.printf("diag uart0 baud=%lu\r\n", baud);
        }
    }
    else if (strcmp(line, "trace on") == 0) { diag_trace_set(true); diag_printf("trace on\r\n"); }
    else if (strcmp(line, "trace off") == 0) { diag_trace_set(false); diag_printf("trace off\r\n"); }
#endif
    else if (strcmp(line, "reboot") == 0) { diag_printf("rebooting\r\n"); delay(100); ESP.restart(); }
    else diag_printf("unknown command\r\n");

    diag_print_prompt();
}

void diag_setup(void)
{
    diag_printf("\r\nespBode %s\r\n", FW_VARIANT_NAME);
    diag_printf("build: %s\r\n", FW_BUILD_STRING);
    diag_printf("stored config valid: %s\r\n", config_store_was_valid() ? "yes" : "no");
    diag_printf("wifi: mode=%s state=%s ip=%s\r\n",
        wifi_mode_name(),
        (WiFi.status() == WL_CONNECTED) ? "connected" : "not-connected",
        wifi_current_ip().toString().c_str());
#if ENABLE_WIFI_DIAG
    diag_printf("wifi_status=%d fail=%s heap=%lu\r\n",
        (int)WiFi.status(),
        wifi_last_fail_reason_text(),
        (unsigned long)ESP.getFreeHeap());
#else
    diag_printf("awg: enabled=%s baud=%lu\r\n",
        fy_is_enabled() ? "yes" : "no",
        (unsigned long)fy_get_baud());
#endif
    diag_print_prompt();
}

void diag_poll(void)
{
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') {
            s_line[s_line_len] = '\0';
            Serial.print("\r\n");
            process_line(s_line);
            s_line_len = 0;
            continue;
        }
        if ((ch == '\b' || ch == 0x7F) && s_line_len > 0) {
            s_line_len--;
            continue;
        }
        if (s_line_len < sizeof(s_line) - 1 && ch >= 32 && ch < 127) {
            s_line[s_line_len++] = ch;
        }
    }
}

#else

void diag_begin_early(void) {}
void diag_setup(void) {}
void diag_poll(void) {}
bool diag_trace_enabled(void) { return false; }
void diag_trace_set(bool enabled) { (void)enabled; }
void diag_tracef(const char *fmt, ...) { (void)fmt; }

#endif