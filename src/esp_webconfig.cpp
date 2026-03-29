#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_webconfig.h"
#include "esp_persist.h"
#include "esp_fy6900.h"
#include "esp_network.h"
#include "esp_wifi.h"
#include "esp_config.h"

/*
 * HTML is stored in program (flash) memory to avoid consuming RAM.
 * The form uses plain HTML with no external resources – suitable for
 * connection via an AP hotspot without internet access.
 */
static const char HTML_HEAD[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>espBode Config</title>"
    "<style>body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:8px}"
    "label{display:block;margin-top:8px;font-size:.9em}"
    "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:4px}"
    "input[type=submit]{margin-top:14px;padding:6px 18px}"
    "pre{background:#f5f5f5;padding:8px;overflow:auto;white-space:pre-wrap;font-family:monospace;font-size:.85em}"
    "h2{margin-bottom:4px}.note{font-size:.8em;color:#666}</style></head><body>"
    "<h2>espBode WiFi Config</h2>";

static const char HTML_FOOT[] PROGMEM =
    "</body></html>";

static ESP8266WebServer s_server(80);

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static String ip_to_str(const uint8_t *ip4)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip4[0], ip4[1], ip4[2], ip4[3]);
    return String(buf);
}

static bool str_to_ip(const String &s, uint8_t *ip4)
{
    unsigned a, b, c, d;
    if (sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    ip4[0] = (uint8_t)a; ip4[1] = (uint8_t)b;
    ip4[2] = (uint8_t)c; ip4[3] = (uint8_t)d;
    return true;
}

static bool is_safe_hostname(const String &s)
{
    size_t len = s.length();
    if (len == 0 || len > sizeof(g_config.device_hostname) - 1) return false;
    if (s[0] == '-' || s[len - 1] == '-') return false;

    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        if (!isalnum((unsigned char)ch) && ch != '-') return false;
    }
    return true;
}

static bool is_safe_label_text(const String &s, size_t max_len, bool allow_space)
{
    size_t len = s.length();
    if (len == 0 || len > max_len) return false;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 32u || ch > 126u) return false;
        if (!allow_space && ch == ' ') return false;
        if (ch == '\'' || ch == '"' || ch == '<' || ch == '>' || ch == '&') return false;
    }
    return true;
}

static bool is_safe_idn_name(const String &s)
{
    size_t len = s.length();
    if (len == 0 || len > sizeof(g_config.idn_response_name) - 1) return false;
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') return false;
    }
    return true;
}

static bool is_safe_mac(const uint8_t *mac)
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

static bool parse_mac(const String &s, uint8_t *mac)
{
    unsigned values[6];

    if (s.length() != 17) return false;
    if (sscanf(s.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)values[i];
    }
    return is_safe_mac(mac);
}

static String mac_to_str(const uint8_t *mac, uint8_t enabled)
{
    char buf[18];

    if (!enabled) return String();
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

static bool parse_u32(const String &s, uint32_t *value)
{
    char *endp = NULL;
    unsigned long parsed;

    if (s.length() == 0) return false;
    parsed = strtoul(s.c_str(), &endp, 10);
    if (endp == NULL || *endp != '\0') return false;
    *value = (uint32_t)parsed;
    return true;
}

#if ENABLE_PROTOCOL_DIAG
static void append_html_escaped(String &page, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        page += F("-");
        return;
    }

    while (*text) {
        switch (*text) {
        case '&': page += F("&amp;"); break;
        case '<': page += F("&lt;"); break;
        case '>': page += F("&gt;"); break;
        default:  page += *text; break;
        }
        text++;
    }
}

static void append_protocol_diag_section(String &page)
{
    const NetStats *stats = net_get_stats();
    char buf[192];

    page += F("<hr><h3>Protocol Diagnostics</h3>");
    page += F("<p class='note'><a href='/config'>Refresh</a> | <a href='/config?clear_proto=1'>Clear protocol log</a></p><pre>");

    page += F("CURRENT STATUS SUMMARY\n");
    snprintf(buf, sizeof(buf),
        "current_vxi_port=%u\nsession_active=%s\nuptime_ms=%lu\n",
        stats->current_vxi_port,
        stats->session_active ? "yes" : "no",
        (unsigned long)millis());
    page += buf;
    page += F("reset_reason=");
    append_html_escaped(page, ESP.getResetReason().c_str());
    page += F("\n");
    page += F("active_client=");
    if (stats->session_active) {
        page += stats->active_client_ip.toString();
        snprintf(buf, sizeof(buf), ":%u\n", stats->active_client_port);
        page += buf;
    } else {
        page += F("-\n");
    }
    page += F("last_event=");
    append_html_escaped(page, stats->last_event);
    page += F("\n");
    snprintf(buf, sizeof(buf),
        "udp_getport_count=%lu\ntcp_getport_count=%lu\ngetport_zero_reply_count=%lu\n"
        "create_link_count=%lu\ndevice_write_count=%lu\ndevice_read_count=%lu\n",
        (unsigned long)stats->udp_getport_count,
        (unsigned long)stats->tcp_getport_count,
        (unsigned long)stats->getport_zero_reply_count,
        (unsigned long)stats->create_link_count,
        (unsigned long)stats->device_write_count,
        (unsigned long)stats->device_read_count);
    page += buf;
    snprintf(buf, sizeof(buf),
        "destroy_link_count=%lu\nsession_accept_count=%lu\nsession_end_count=%lu\n"
        "session_drop_count=%lu\nunknown_proc_count=%lu\nmalformed_packet_count=%lu\n",
        (unsigned long)stats->destroy_link_count,
        (unsigned long)stats->session_accept_count,
        (unsigned long)stats->session_end_count,
        (unsigned long)stats->session_drop_count,
        (unsigned long)stats->unknown_proc_count,
        (unsigned long)stats->malformed_packet_count);
    page += buf;
    snprintf(buf, sizeof(buf),
        "last_write_len=%lu\nlast_read_len=%lu\nlast_read_declared_len=%lu\n"
        "last_read_mode=%s\n\n",
        (unsigned long)stats->last_write_len,
        (unsigned long)stats->last_read_len,
        (unsigned long)stats->last_read_declared_len,
        stats->last_read_fixed_id ? "fixed_id" : "parser");
    page += buf;
    snprintf(buf, sizeof(buf),
        "ch1.load=%s\nch1.amp.requested=%.4f\nch1.amp.drive=%.4f\n"
        "ch1.offset.requested=%.4f\nch1.offset.drive=%.4f\n",
        fy_load_to_text(fy_get_load(1)),
        g_awg.ch1_ampl_v,
        fy_get_drive_ampl(1),
        g_awg.ch1_ofst_v,
        fy_get_drive_offset(1));
    page += buf;
    snprintf(buf, sizeof(buf),
        "ch2.load=%s\nch2.amp.requested=%.4f\nch2.amp.drive=%.4f\n"
        "ch2.offset.requested=%.4f\nch2.offset.drive=%.4f\n\n",
        fy_load_to_text(fy_get_load(2)),
        g_awg.ch2_ampl_v,
        fy_get_drive_ampl(2),
        g_awg.ch2_ofst_v,
        fy_get_drive_offset(2));
    page += buf;

    page += F("LAST PARSED REQUEST DETAILS\n");
    page += F("last_rpc_transport=");
    append_html_escaped(page, stats->last_rpc_transport);
    page += F("\n");
    if (stats->last_getport_valid) {
        snprintf(buf, sizeof(buf),
            "last_getport.map_prog=0x%08lX\nlast_getport.map_vers=%lu\n"
            "last_getport.map_proto=%lu\nlast_getport.reply_port=%u\n",
            (unsigned long)stats->last_map_prog,
            (unsigned long)stats->last_map_vers,
            (unsigned long)stats->last_map_proto,
            stats->last_getport_reply);
        page += buf;
    } else {
        page += F("last_getport=-\n");
    }
    if (stats->last_vxi_valid) {
        snprintf(buf, sizeof(buf),
            "last_vxi.program=0x%08lX\nlast_vxi.version=%lu\n"
            "last_vxi.procedure=%lu\nlast_vxi.xid=0x%08lX\n",
            (unsigned long)stats->last_vxi_program,
            (unsigned long)stats->last_vxi_version,
            (unsigned long)stats->last_vxi_procedure,
            (unsigned long)stats->last_vxi_xid);
        page += buf;
    } else {
        page += F("last_vxi=-\n");
    }
    if (stats->last_create_link_valid) {
        snprintf(buf, sizeof(buf),
            "last_create_link.clientId=%lu\nlast_create_link.lockDevice=%u\n"
            "last_create_link.lock_timeout=%lu\nlast_create_link.device=",
            (unsigned long)stats->last_create_client_id,
            (unsigned)stats->last_create_lock_device,
            (unsigned long)stats->last_create_lock_timeout);
        page += buf;
        append_html_escaped(page, stats->last_create_device);
        page += F("\n");
    } else {
        page += F("last_create_link=-\n");
    }

    page += F("\nRECENT EVENT LOG\n");
    if (stats->recent_event_count == 0) {
        page += F("-\n");
    } else {
        uint8_t start = (uint8_t)((stats->recent_event_head + NET_PROTO_EVENT_COUNT
            - stats->recent_event_count) % NET_PROTO_EVENT_COUNT);
        for (uint8_t i = 0; i < stats->recent_event_count; i++) {
            uint8_t idx = (uint8_t)((start + i) % NET_PROTO_EVENT_COUNT);
            append_html_escaped(page, stats->recent_events[idx]);
            page += F("\n");
        }
    }

    page += F("</pre>");
}
#endif

/* ── GET / ──────────────────────────────────────────────────────────────── */
static void handle_root(void)
{
    s_server.sendHeader("Location", "/config", true);
    s_server.send(302, "text/plain", "");
}

static void append_status_summary(String &page)
{
    page += F("<p class='note'><b>");
    if (!wifi_is_sta()) {
        page += F("Fallback AP active");
    } else {
        page += F("WiFi status");
    }
    page += F("</b><br>Current mode: ");
    page += wifi_mode_text();
    page += F(" | station status: ");
    page += wifi_is_connected() ? F("connected") : F("not connected");
    page += F(" | IP: ");
    page += wifi_current_ip().toString();
    page += F(" | Fail: ");
    page += wifi_last_fail_reason_text();
    page += F("</p>");
}

/* ── GET /config ─────────────────────────────────────────────────────────── */
static void handle_config_get(void)
{
#if ENABLE_PROTOCOL_DIAG
    if (s_server.hasArg("clear_proto")) {
        net_clear_protocol_diag();
        s_server.sendHeader("Location", "/config", true);
        s_server.send(302, "text/plain", "");
        return;
    }
#endif

    String mode_sta = g_config.use_dhcp ? "" : " checked";
    String mode_dhcp = g_config.use_dhcp ? " checked" : "";

    String page = FPSTR(HTML_HEAD);
    page.reserve(4600);
    append_status_summary(page);
    page += F("<p class='note'>Friendly Name: <b>");
    page += String(g_config.friendly_name);
    page += F("</b> | Active IDN reply: <b>");
    page += String(IDN_RESPONSE_PREFIX);
    page += String(g_config.idn_response_name);
    page += F("</b></p>");
    page += F("<form method='post' action='/config'>"
              "<label>SSID</label>"
              "<input type='text' name='ssid' maxlength='32' value='");
    page += String(g_config.ssid);
    page += F("'><label>PSK (WiFi password)</label>"
              "<input type='password' name='psk' maxlength='64' value='");
    page += String(g_config.psk);
    page += F("'><label>Device Hostname</label>"
              "<input type='text' name='device_hostname' maxlength='24' value='");
    page += String(g_config.device_hostname);
    page += F("'><label>Friendly Name</label>"
              "<input type='text' name='friendly_name' maxlength='24' value='");
    page += String(g_config.friendly_name);
    page += F("'><label>IDN Model Name</label>"
              "<input type='text' name='idn_response_name' maxlength='16' value='");
    page += String(g_config.idn_response_name);
    page += F("'><hr><label><input type='radio' name='dhcp' value='0'");
    page += mode_sta;
    page += F("> Static IP</label>"
              "<label><input type='radio' name='dhcp' value='1'");
    page += mode_dhcp;
    page += F("> DHCP</label>"
              "<label>IP Address</label>"
              "<input type='text' name='ip' value='");
    page += ip_to_str(g_config.ip);
    page += F("'><label>Subnet Mask</label>"
              "<input type='text' name='mask' value='");
    page += ip_to_str(g_config.mask);
    page += F("'><label>Gateway</label>"
              "<input type='text' name='gw' value='");
    page += ip_to_str(g_config.gw);
    page += F("'><label>DNS</label>"
              "<input type='text' name='dns' value='");
    page += ip_to_str(g_config.dns);
    page += F("'><hr><p class='note'>Fallback AP: ESP01 | open | 192.168.0.1/config</p>"
              "<label>Station MAC Override</label>"
              "<input type='text' name='sta_mac' maxlength='17' placeholder='XX:XX:XX:XX:XX:XX' value='");
    page += mac_to_str(g_config.sta_mac, g_config.sta_mac_set);
    page += F("'>");
#if ENABLE_AP_MAC_OVERRIDE
    page += F("<label>AP MAC Override</label>"
              "<input type='text' name='ap_mac' maxlength='17' placeholder='XX:XX:XX:XX:XX:XX' value='");
    page += mac_to_str(g_config.ap_mac, g_config.ap_mac_set);
    page += F("'>");
#endif
    page += F("<label>AWG UART Baud</label>"
              "<input type='text' name='awg_baud' maxlength='6' value='");
    page += String((unsigned long)g_config.awg_baud);
    page += F("'><br><input type='submit' value='Save &amp; Reboot'>"
              "</form>"
              "<hr><p class='note'>"
              "<a href='/retry'>Reboot &amp; Retry WiFi</a> | "
              "<a href='/reset'>Restore compile-time defaults</a> | "
              "Current mode: ");
    page += wifi_mode_text();
    page += F(" | Current IP: ");
    page += wifi_current_ip().toString();
    page += F(" | WiFi, MAC and baud changes apply after reboot; IDN reply uses the current config</p>");
#if ENABLE_PROTOCOL_DIAG
    append_protocol_diag_section(page);
#endif
    page += FPSTR(HTML_FOOT);

    s_server.send(200, "text/html", page);
}

static void handle_retry(void)
{
    s_server.send(200, "text/html",
        F("<!DOCTYPE html><html><body>"
          "<h2>Retrying WiFi</h2><p>Rebooting in 2 seconds&hellip;</p>"
          "</body></html>"));
    delay(2000);
    ESP.restart();
}

static void handle_reset(void)
{
    if (s_server.arg("confirm") != "yes") {
        s_server.send(200, "text/html",
            F("<!DOCTYPE html><html><body>"
              "<h2>Factory Reset</h2>"
              "<p>This restores the compile-time defaults currently built into the firmware.</p>"
              "<p><a href='/reset?confirm=yes'>Confirm reset</a></p>"
              "<p><a href='/config'>&larr; Back</a></p>"
              "</body></html>"));
        return;
    }

    config_reset_defaults();
    config_save();

    s_server.send(200, "text/html",
        F("<!DOCTYPE html><html><body>"
          "<h2>Defaults Restored</h2><p>Rebooting in 2 seconds&hellip;</p>"
          "</body></html>"));

    delay(2000);
    ESP.restart();
}

/* ── POST /config ────────────────────────────────────────────────────────── */
static void handle_config_post(void)
{
    EspConfig next = g_config;
    bool ok = true;
    String err;
    uint32_t baud = 0;

    /* SSID */
    if (s_server.hasArg("ssid") && s_server.arg("ssid").length() > 0) {
        String ssid = s_server.arg("ssid");
        if (ssid.length() > 32) { ok = false; err += "SSID too long. "; }
        else strncpy(next.ssid, ssid.c_str(), sizeof(next.ssid) - 1);
    } else { ok = false; err += "SSID required. "; }

    /* PSK (allow empty for open networks) */
    if (s_server.hasArg("psk")) {
        String psk = s_server.arg("psk");
        if (psk.length() > 64) { ok = false; err += "PSK too long. "; }
        else strncpy(next.psk, psk.c_str(), sizeof(next.psk) - 1);
    }

    if (s_server.hasArg("device_hostname")) {
        String hostname = s_server.arg("device_hostname");
        if (!is_safe_hostname(hostname)) { ok = false; err += "Invalid hostname. "; }
        else strncpy(next.device_hostname, hostname.c_str(), sizeof(next.device_hostname) - 1);
    }

    if (s_server.hasArg("friendly_name")) {
        String friendly = s_server.arg("friendly_name");
        if (!is_safe_label_text(friendly, sizeof(next.friendly_name) - 1, true)) {
            ok = false; err += "Invalid friendly name. ";
        } else {
            strncpy(next.friendly_name, friendly.c_str(), sizeof(next.friendly_name) - 1);
        }
    }

    if (s_server.hasArg("idn_response_name")) {
        String idn_name = s_server.arg("idn_response_name");
        if (!is_safe_idn_name(idn_name)) { ok = false; err += "Invalid IDN model name. "; }
        else strncpy(next.idn_response_name, idn_name.c_str(), sizeof(next.idn_response_name) - 1);
    }

    /* DHCP flag */
    if (s_server.hasArg("dhcp")) {
        next.use_dhcp = (s_server.arg("dhcp") == "1") ? 1 : 0;
    }

    /* Static IP fields (only validated when static mode selected) */
    if (!next.use_dhcp) {
        if (!s_server.hasArg("ip")   || !str_to_ip(s_server.arg("ip"),   next.ip))
            { ok = false; err += "Invalid IP. "; }
        if (!s_server.hasArg("mask") || !str_to_ip(s_server.arg("mask"), next.mask))
            { ok = false; err += "Invalid mask. "; }
        if (!s_server.hasArg("gw")   || !str_to_ip(s_server.arg("gw"),   next.gw))
            { ok = false; err += "Invalid gateway. "; }
        if (!s_server.hasArg("dns")  || !str_to_ip(s_server.arg("dns"),  next.dns))
            { ok = false; err += "Invalid DNS. "; }
    }

    if (s_server.hasArg("sta_mac")) {
        String sta_mac = s_server.arg("sta_mac");
        if (sta_mac.length() == 0) {
            next.sta_mac_set = 0;
            memset(next.sta_mac, 0, sizeof(next.sta_mac));
        } else if (!parse_mac(sta_mac, next.sta_mac)) {
            ok = false; err += "Invalid station MAC. ";
        } else {
            next.sta_mac_set = 1;
        }
    }

#if ENABLE_AP_MAC_OVERRIDE
    if (s_server.hasArg("ap_mac")) {
        String ap_mac = s_server.arg("ap_mac");
        if (ap_mac.length() == 0) {
            next.ap_mac_set = 0;
            memset(next.ap_mac, 0, sizeof(next.ap_mac));
        } else if (!parse_mac(ap_mac, next.ap_mac)) {
            ok = false; err += "Invalid AP MAC. ";
        } else {
            next.ap_mac_set = 1;
        }
    }
#endif

    if (s_server.hasArg("awg_baud")) {
        String awg_baud = s_server.arg("awg_baud");
        if (awg_baud.length() == 0) {
            next.awg_baud = AWG_BAUD_RATE;
        } else if (!parse_u32(awg_baud, &baud) || !fy_is_supported_baud(baud)) {
            ok = false; err += "Unsupported AWG baud. ";
        } else {
            next.awg_baud = baud;
        }
    }

    if (!ok) {
        String page = FPSTR(HTML_HEAD);
        page += F("<p style='color:red'>Errors: ");
        page += err;
        page += F("</p><a href='/config'>&larr; Back</a>");
        page += FPSTR(HTML_FOOT);
        s_server.send(400, "text/html", page);
        return;
    }

    g_config = next;
    config_save();

    s_server.send(200, "text/html",
        F("<!DOCTYPE html><html><body>"
          "<h2>Saved!</h2><p>Rebooting in 2 seconds&hellip;</p>"
          "</body></html>"));

    delay(2000);
    ESP.restart();
}

/* ── GET /reset ──────────────────────────────────────────────────────────── */
/* ── GET /diag (raw AWG command test) ────────────────────────────────────── */
#ifdef DEBUG_BUILD
#include "esp_fy6900.h"
static void handle_diag(void)
{
    String page = FPSTR(HTML_HEAD);
    page += F("<h3>AWG Diagnostic</h3>"
              "<form method='get' action='/diag'>"
              "Raw command: <input type='text' name='cmd' size='24'>"
              "<input type='submit' value='Send'></form>");

    if (s_server.hasArg("cmd")) {
        char reply[64];
        String cmd = s_server.arg("cmd");
        int n = fy_raw_cmd(cmd.c_str(), reply, sizeof(reply));
        page += F("<p>Response (");
        page += String(n);
        page += F(" bytes): <code>");
        page += String(reply);
        page += F("</code></p>");
    }
    page += FPSTR(HTML_FOOT);
    s_server.send(200, "text/html", page);
}
#endif

/* ── Public API ──────────────────────────────────────────────────────────── */
void webconfig_begin(void)
{
    s_server.on("/",       HTTP_GET,  handle_root);
    s_server.on("/config", HTTP_GET,  handle_config_get);
    s_server.on("/config", HTTP_POST, handle_config_post);
    s_server.on("/retry",  HTTP_GET,  handle_retry);
    s_server.on("/reset",  HTTP_GET,  handle_reset);
#ifdef DEBUG_BUILD
    s_server.on("/diag",   HTTP_GET,  handle_diag);
#endif
    s_server.begin();
    DBG("Web config server started on port 80");
}

void webconfig_poll(void)
{
    s_server.handleClient();
}
