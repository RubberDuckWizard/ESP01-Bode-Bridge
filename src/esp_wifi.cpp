#include <ESP8266WiFi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <user_interface.h>
#include "esp_diag_console.h"
#include "esp_wifi.h"
#include "esp_persist.h"
#include "esp_config.h"

static bool s_sta_mode = false;
static WifiFailReason s_last_fail_reason = WIFI_FAIL_NONE;

static bool ip_is_zero(IPAddress ip)
{
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

static WifiFailReason map_status_reason(wl_status_t status)
{
    switch (status) {
        case WL_NO_SSID_AVAIL:  return WIFI_FAIL_SSID_NOT_FOUND;
        case WL_WRONG_PASSWORD: return WIFI_FAIL_AUTH_FAILED;
        case WL_CONNECT_FAILED: return WIFI_FAIL_CONNECT_FAILED;
        default:                return WIFI_FAIL_TIMEOUT;
    }
}

static void wifi_diag_log(const char *fmt, ...)
{
#if ENABLE_WIFI_DIAG
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    diag_tracef("%s", buf);
#else
    (void)fmt;
#endif
}

static const char *reason_text(WifiFailReason reason)
{
    switch (reason) {
        case WIFI_FAIL_NONE:             return "none";
        case WIFI_FAIL_TIMEOUT:          return "timeout";
        case WIFI_FAIL_SSID_NOT_FOUND:   return "ssid_not_found";
        case WIFI_FAIL_AUTH_FAILED:      return "auth_failed";
        case WIFI_FAIL_CONNECT_FAILED:   return "connect_failed";
        case WIFI_FAIL_DHCP_FAILED:      return "dhcp_failed";
        case WIFI_FAIL_STATIC_IP_FAILED: return "static_ip_failed";
        case WIFI_FAIL_INVALID_CONFIG:   return "invalid_config";
        default:                         return "unknown";
    }
}

static bool mac_is_safe(const uint8_t *mac)
{
    bool any_nonzero = false;
    bool all_ff = true;

    if ((mac[0] & 0x01u) != 0) return false;

    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00u) any_nonzero = true;
        if (mac[i] != 0xFFu) all_ff = false;
    }
    return any_nonzero && !all_ff;
}

static const char *cfg_hostname(void)
{
    return g_config.device_hostname[0] ? g_config.device_hostname : DEF_DEVICE_HOSTNAME;
}

static void apply_mac_override(uint8_t ifx, const uint8_t *mac)
{
    uint8_t copy[6];

    if (!mac_is_safe(mac)) return;
    memcpy(copy, mac, sizeof(copy));
    wifi_set_macaddr(ifx, copy);
}

static void config_ap_dhcp_range(void)
{
    static const uint8_t ap_start[] = { AP_DHCP_START_BYTES };
    static const uint8_t ap_end[] = { AP_DHCP_END_BYTES };
    struct dhcps_lease lease;

    memset(&lease, 0, sizeof(lease));
    lease.enable = true;
    IP4_ADDR(&lease.start_ip, ap_start[0], ap_start[1], ap_start[2], ap_start[3]);
    IP4_ADDR(&lease.end_ip, ap_end[0], ap_end[1], ap_end[2], ap_end[3]);

    bool stop_ok = wifi_softap_dhcps_stop();
    bool lease_ok = wifi_softap_set_dhcps_lease(&lease);
    bool start_ok = wifi_softap_dhcps_start();

    wifi_diag_log("dhcp stop=%d lease=%d start=%d range=%u.%u.%u.%u-%u.%u.%u.%u\r\n",
        stop_ok ? 1 : 0,
        lease_ok ? 1 : 0,
        start_ok ? 1 : 0,
        ap_start[0], ap_start[1], ap_start[2], ap_start[3],
        ap_end[0], ap_end[1], ap_end[2], ap_end[3]);
}

static bool start_fallback_ap(WifiFailReason reason)
{
    IPAddress apIP(AP_IP_BYTES);
    IPAddress apMask(AP_MASK_BYTES);
    bool mode_ok;
    bool cfg_ok;
    bool ap_ok;

    s_last_fail_reason = reason;
    WiFi.disconnect();
    mode_ok = WiFi.mode(WIFI_AP);
    wifi_diag_log("wifi.mode(AP)=%d current_mode=%d\r\n", mode_ok ? 1 : 0, (int)WiFi.getMode());

#if ENABLE_AP_MAC_OVERRIDE
    if (g_config.ap_mac_set) {
        apply_mac_override(SOFTAP_IF, g_config.ap_mac);
    }
#endif

    cfg_ok = WiFi.softAPConfig(apIP, apIP, apMask);
    wifi_diag_log("WiFi.softAPConfig(ip=%s mask=%s)=%d actual_ip=%s\r\n",
        apIP.toString().c_str(),
        apMask.toString().c_str(),
        cfg_ok ? 1 : 0,
        WiFi.softAPIP().toString().c_str());

    ap_ok = WiFi.softAP(AP_SSID);
    wifi_diag_log("WiFi.softAP(ssid=%s open)= %d actual_ssid=%s ap_ip=%s ap_mac=%s\r\n",
        AP_SSID,
        ap_ok ? 1 : 0,
        WiFi.softAPSSID().c_str(),
        WiFi.softAPIP().toString().c_str(),
        WiFi.softAPmacAddress().c_str());

    config_ap_dhcp_range();

    wifi_diag_log("final_ap_result fail=%s ap_ip=%s ap_mac=%s\r\n",
        reason_text(reason),
        WiFi.softAPIP().toString().c_str(),
        WiFi.softAPmacAddress().c_str());

    DBGF("AP mode: SSID=%s open reason=%s IP=192.168.0.1\n",
         AP_SSID, reason_text(reason));
    s_sta_mode = false;
    return false;
}

bool wifi_setup()
{
    wl_status_t status;
    wl_status_t begin_status;
    int last_status = -999;
    bool mode_ok;
    bool host_ok;

    WiFi.persistent(false);   /* do not store credentials in SDK flash area */
    mode_ok = WiFi.mode(WIFI_STA);
    wifi_diag_log("wifi.mode(STA)=%d current_mode=%d\r\n", mode_ok ? 1 : 0, (int)WiFi.getMode());
    host_ok = WiFi.hostname(cfg_hostname());
    wifi_diag_log("WiFi.hostname(%s)=%d\r\n", cfg_hostname(), host_ok ? 1 : 0);
    s_last_fail_reason = WIFI_FAIL_NONE;

    if (g_config.sta_mac_set) {
        apply_mac_override(STATION_IF, g_config.sta_mac);
        wifi_diag_log("sta_mac_override=%s\r\n", WiFi.macAddress().c_str());
    }

    if (g_config.ssid[0] == '\0') {
        wifi_diag_log("wifi invalid config: empty ssid\r\n");
        return start_fallback_ap(WIFI_FAIL_INVALID_CONFIG);
    }

    /* Apply static IP before begin() if DHCP not requested */
    if (!g_config.use_dhcp) {
        IPAddress ip  (g_config.ip[0],   g_config.ip[1],   g_config.ip[2],   g_config.ip[3]);
        IPAddress mask(g_config.mask[0], g_config.mask[1], g_config.mask[2], g_config.mask[3]);
        IPAddress gw  (g_config.gw[0],   g_config.gw[1],   g_config.gw[2],   g_config.gw[3]);
        IPAddress dns (g_config.dns[0],  g_config.dns[1],  g_config.dns[2],  g_config.dns[3]);
        bool cfg_ok = WiFi.config(ip, gw, mask, dns);
        wifi_diag_log("WiFi.config(ip=%s gw=%s mask=%s dns=%s)=%d\r\n",
            ip.toString().c_str(), gw.toString().c_str(), mask.toString().c_str(), dns.toString().c_str(),
            cfg_ok ? 1 : 0);
        if (!cfg_ok) {
            return start_fallback_ap(WIFI_FAIL_STATIC_IP_FAILED);
        }
    } else {
        wifi_diag_log("WiFi.config skipped (dhcp=on)\r\n");
    }

    begin_status = WiFi.begin(g_config.ssid, g_config.psk);
    wifi_diag_log("WiFi.begin(ssid=%s, psk_len=%u)=%d\r\n",
        g_config.ssid,
        (unsigned)strlen(g_config.psk),
        (int)begin_status);
    DBGF("Connecting to SSID: %s\n", g_config.ssid);

    uint32_t t0 = millis();
    while ((status = WiFi.status()) != WL_CONNECTED) {
        if ((int)status != last_status) {
            wifi_diag_log("WiFi.status=%d fail_guess=%s\r\n", (int)status, reason_text(map_status_reason(status)));
            last_status = (int)status;
        }
        if ((millis() - t0) > STA_TIMEOUT_MS) {
            wifi_diag_log("wifi sta timeout after %lu ms status=%d\r\n",
                (unsigned long)(millis() - t0), (int)status);
            return start_fallback_ap(map_status_reason(status));
        }
        delay(200);
        yield();
    }

    wifi_diag_log("WiFi.status=%d connected\r\n", (int)status);

    while (g_config.use_dhcp && ip_is_zero(WiFi.localIP())) {
        if ((millis() - t0) > STA_TIMEOUT_MS) {
            wifi_diag_log("dhcp timeout after %lu ms ip=%s\r\n",
                (unsigned long)(millis() - t0), WiFi.localIP().toString().c_str());
            return start_fallback_ap(WIFI_FAIL_DHCP_FAILED);
        }
        delay(50);
        yield();
    }

    wifi_diag_log("final_sta_result connected ip=%s gw=%s dns=%s heap=%lu\r\n",
        WiFi.localIP().toString().c_str(),
        WiFi.gatewayIP().toString().c_str(),
        WiFi.dnsIP().toString().c_str(),
        (unsigned long)ESP.getFreeHeap());
    DBGF("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    s_sta_mode = true;
    s_last_fail_reason = WIFI_FAIL_NONE;
    return true;
}

bool wifi_is_sta()
{
    return s_sta_mode;
}

bool wifi_is_connected()
{
    return s_sta_mode && WiFi.status() == WL_CONNECTED && !ip_is_zero(WiFi.localIP());
}

WifiFailReason wifi_last_fail_reason()
{
    return s_last_fail_reason;
}

const char *wifi_last_fail_reason_text()
{
    return reason_text(s_last_fail_reason);
}

const char *wifi_mode_text()
{
    return s_sta_mode ? "STA" : "AP fallback";
}

IPAddress wifi_current_ip()
{
    return s_sta_mode ? WiFi.localIP() : WiFi.softAPIP();
}

bool wifi_start_fallback_ap_for_diag(void)
{
    return start_fallback_ap(s_last_fail_reason == WIFI_FAIL_NONE ? WIFI_FAIL_TIMEOUT : s_last_fail_reason);
}
