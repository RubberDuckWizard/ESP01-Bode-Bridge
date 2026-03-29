#include <Arduino.h>
#include <ctype.h>
#include <stdarg.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include "esp_network.h"
#include "esp_config.h"
#include "esp_diag_console.h"
#include "esp_fy6900.h"
#include "esp_parser.h"

/* ── XDR / RPC byte-order helpers (no struct casts, no double-swap) ──────── */
static inline uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v      );
}

/* ── RPC reply header helpers ────────────────────────────────────────────── */
/*
 * Layout (all uint32_t, big-endian, with TCP frag marker at start):
 *   [0..3]   frag   = 0x80000000 | (bytes after frag field)
 *   [4..7]   xid
 *   [8..11]  msg_type = 1 (REPLY)
 *   [12..15] reply_state = 0 (MSG_ACCEPTED)
 *   [16..19] verif_flavor = 0 (AUTH_NONE)
 *   [20..23] verif_len    = 0
 *   [24..27] accept_stat  = 0 (SUCCESS)
 *   [28..]   procedure-specific payload
 */
#define RPC_REPLY_HDR_LEN  28   /* frag + 6 × uint32_t */

static void build_reply_hdr(uint8_t *buf, uint32_t xid, uint32_t payload_len)
{
    uint32_t after_frag = 24 + payload_len;   /* from xid to end */
    put_u32(buf +  0, RPC_SINGLE_FRAG | after_frag);
    put_u32(buf +  4, xid);
    put_u32(buf +  8, RPC_REPLY);
    put_u32(buf + 12, MSG_ACCEPTED);
    put_u32(buf + 16, 0);   /* verif flavor = AUTH_NONE */
    put_u32(buf + 20, 0);   /* verif len    = 0         */
    put_u32(buf + 24, SUCCESS_STAT);
}

/* ── RPC request header parsing ─────────────────────────────────────────── */
/*
 * TCP record marking format:
 *   bytes 0..3  frag mark (bit31=last, bits30:0 = bytes that follow)
 * After frag:
 *   +0  xid          +4  msg_type     +8  rpc_version  +12 program
 *   +16 prog_ver     +20 procedure    +24 cred_flavor   +28 cred_len
 *   +32 (cred_data if cred_len>0)
 *   +32 verif_flavor  +36 verif_len
 *   +40 payload
 * (Assumes AUTH_NONE with cred_len=0, which is what the scope sends.)
 */
#define RPC_HDR_OFFSET     4    /* skip frag word to get to XID */
#define PAYLOAD_OFFSET     44   /* 4(frag)+10×4(header fields) */

/* ── Servers ─────────────────────────────────────────────────────────────── */
static WiFiUDP    s_udp;
static WiFiServer s_rpc_tcp(RPC_PORT);
static WiFiServer s_vxi_a(VXI_PORT_A);
static WiFiServer s_vxi_b(VXI_PORT_B);

static uint16_t   s_cur_vxi_port = VXI_PORT_A;
static WiFiClient s_vxi_client;
static bool       s_session_active = false;
static NetStats   s_stats;
static uint32_t   s_last_control_ms = 0;
static uint32_t   s_last_scope_activity_ms = 0;
static bool       s_scope_activity_seen = false;
static bool       s_auto_output_off_done = false;

#define AWG_RESYNC_IDLE_MS 1000U

#if ENABLE_PROTOCOL_DIAG
static void proto_diag_copy_string(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) return;
    if (src == NULL) src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void proto_diag_push_event(const char *text)
{
    uint8_t idx = s_stats.recent_event_head;
    proto_diag_copy_string(s_stats.recent_events[idx], NET_PROTO_EVENT_LEN, text);
    s_stats.recent_event_head = (uint8_t)((idx + 1u) % NET_PROTO_EVENT_COUNT);
    if (s_stats.recent_event_count < NET_PROTO_EVENT_COUNT) {
        s_stats.recent_event_count++;
    }
}

static void proto_diag_set_transport(const char *transport)
{
    proto_diag_copy_string(s_stats.last_rpc_transport,
        sizeof(s_stats.last_rpc_transport), transport);
}

static void proto_diag_sanitize_preview(const uint8_t *src, uint32_t len,
    char *dst, size_t dst_len)
{
    size_t out_len;

    if (dst_len == 0) return;
    if (src == NULL || len == 0) {
        dst[0] = '\0';
        return;
    }

    out_len = (size_t)len;
    if (out_len > dst_len - 1) out_len = dst_len - 1;

    for (size_t i = 0; i < out_len; i++) {
        uint8_t ch = src[i];
        if (ch < 32u || ch > 126u || ch == '<' || ch == '>' || ch == '&' || ch == '\'' || ch == '"') {
            dst[i] = '.';
        } else {
            dst[i] = (char)ch;
        }
    }
    dst[out_len] = '\0';
}

static void proto_diag_record_getport(const char *transport,
    uint32_t map_prog, uint32_t map_vers, uint32_t map_proto, uint32_t reply_port)
{
    proto_diag_set_transport(transport);
    s_stats.last_getport_valid = true;
    s_stats.last_map_prog = map_prog;
    s_stats.last_map_vers = map_vers;
    s_stats.last_map_proto = map_proto;
    s_stats.last_getport_reply = (uint16_t)reply_port;
}

static void proto_diag_record_vxi_call(uint32_t program, uint32_t version,
    uint32_t procedure, uint32_t xid)
{
    proto_diag_set_transport("TCP");
    s_stats.last_vxi_valid = true;
    s_stats.last_vxi_program = program;
    s_stats.last_vxi_version = version;
    s_stats.last_vxi_procedure = procedure;
    s_stats.last_vxi_xid = xid;
}

static bool proto_diag_parse_create_link(const uint8_t *rx, uint32_t rx_len,
    uint32_t *client_id, uint32_t *lock_device, uint32_t *lock_timeout,
    char *device, size_t device_len)
{
    uint32_t device_xdr_len;
    uint32_t padded_len;

    if (rx_len < (uint32_t)(PAYLOAD_OFFSET + 16)) return false;

    *client_id = get_u32(rx + PAYLOAD_OFFSET + 0);
    *lock_device = get_u32(rx + PAYLOAD_OFFSET + 4);
    *lock_timeout = get_u32(rx + PAYLOAD_OFFSET + 8);
    device_xdr_len = get_u32(rx + PAYLOAD_OFFSET + 12);
    padded_len = (device_xdr_len + 3u) & ~3u;

    if ((uint32_t)(PAYLOAD_OFFSET + 16) + padded_len > rx_len) return false;

    proto_diag_sanitize_preview(rx + PAYLOAD_OFFSET + 16, device_xdr_len,
        device, device_len);
    return true;
}
#endif

static void net_set_event(const char *fmt, ...)
{
    char line[NET_PROTO_EVENT_LEN];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    strncpy(s_stats.last_event, line, sizeof(s_stats.last_event) - 1);
    s_stats.last_event[sizeof(s_stats.last_event) - 1] = '\0';
#if ENABLE_PROTOCOL_DIAG
    proto_diag_push_event(line);
#endif
    diag_tracef("[net] %s\r\n", s_stats.last_event);
}

static char ascii_upper_char(char ch)
{
    if (ch >= 'a' && ch <= 'z') return (char)(ch - ('a' - 'A'));
    return ch;
}

static bool contains_ci(const char *haystack, const char *needle)
{
    size_t needle_len;

    if (haystack == NULL || needle == NULL) return false;
    needle_len = strlen(needle);
    if (needle_len == 0) return false;

    for (; *haystack; haystack++) {
        size_t i = 0;
        while (i < needle_len && haystack[i] != '\0'
                && ascii_upper_char(haystack[i]) == ascii_upper_char(needle[i])) {
            i++;
        }
        if (i == needle_len) return true;
    }
    return false;
}

static bool should_force_awg_resync_setup(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0') return false;
    if (contains_ci(cmd, "IDN-SGLT-PRI?")) return true;
    if (contains_ci(cmd, "OUTP LOAD,")) return true;
    if (contains_ci(cmd, "BSWV WVTP,")) return true;
    return false;
}

static bool should_force_awg_resync_idle(void)
{
    if (s_last_control_ms == 0) return false;
    return (uint32_t)(millis() - s_last_control_ms) >= AWG_RESYNC_IDLE_MS;
}

static void note_scope_activity(void)
{
    s_last_scope_activity_ms = millis();
    s_scope_activity_seen = true;
    s_auto_output_off_done = false;
}

static void handle_auto_output_off(void)
{
    if (!s_scope_activity_seen || s_auto_output_off_done) return;
    if ((uint32_t)(millis() - s_last_scope_activity_ms) < AUTO_OUTPUT_OFF_TIMEOUT_MS) return;

    s_auto_output_off_done = true;
    if (g_awg.ch1_on == 0) return;

    fy_set_output(1, 0);
    net_set_event("auto ch1 off after 30s");
}

/* Safe VXI DEVICE_WRITE payload capacity within RX_BUF_SIZE.
 * Total record layout is:
 *   4 bytes frag + 40 bytes RPC call header + 20 bytes VXI write args + data
 * Therefore the largest accepted SCPI write payload is RX_BUF_SIZE - 64.
 */
#define VXI_WRITE_DATA_OFFSET   (PAYLOAD_OFFSET + 20)
#define VXI_MAX_RECV_SIZE       (RX_BUF_SIZE - VXI_WRITE_DATA_OFFSET)

static uint16_t getport_reply_port(uint32_t map_prog, uint32_t map_vers, uint32_t map_proto)
{
    /* Accept only the exact VXI-11 core mapping on TCP.
     * Portmapper protocols use IPPROTO_TCP=6 and IPPROTO_UDP=17.
     * VXI-11 core itself runs on TCP only.
     */
    if (map_prog != VXI11_CORE_PROG) return 0;
    if (map_vers != VXI11_CORE_VERS) return 0;
    if (map_proto != 6u) return 0;
    return s_cur_vxi_port;
}

/* ── Rotate VXI port ─────────────────────────────────────────────────────── */
static void rotate_vxi_port(void)
{
    s_cur_vxi_port = (s_cur_vxi_port == VXI_PORT_A) ? VXI_PORT_B : VXI_PORT_A;
    s_stats.current_vxi_port = s_cur_vxi_port;
    DBGF("VXI port rotated -> %u\n", s_cur_vxi_port);
}

/* ── Read exactly n bytes from TCP client with timeout ──────────────────── */
static bool tcp_read_n(WiFiClient &c, uint8_t *buf, size_t n)
{
    uint32_t t0 = millis();
    size_t   got = 0;
    while (got < n) {
        if (!c.connected()) return false;
        if ((millis() - t0) > VXI_SESSION_TIMEOUT_MS) return false;
        int avail = c.available();
        if (avail <= 0) { yield(); continue; }
        size_t want = n - got;
        if ((size_t)avail < want) want = (size_t)avail;
        size_t r = c.readBytes(buf + got, want);
        got += r;
    }
    return true;
}

/* ── Read a complete TCP RPC record into rx_buf ─────────────────────────── */
/*
 * Returns total byte count (including 4-byte frag word) or 0 on error.
 * rx_buf must be at least RX_BUF_SIZE bytes.
 */
static uint32_t read_rpc_tcp(WiFiClient &c, uint8_t *rx_buf)
{
    uint8_t frag_bytes[4];
    if (!tcp_read_n(c, frag_bytes, 4)) return 0;

    uint32_t frag      = get_u32(frag_bytes);
    uint32_t data_len  = frag & 0x7FFFFFFFul;

    if (data_len > (uint32_t)(RX_BUF_SIZE - 4)) {
        /* Packet too large – drain and discard */
        DBGF("RPC: oversize packet %u bytes, draining\n", data_len);
#if ENABLE_PROTOCOL_DIAG
        s_stats.malformed_packet_count++;
        net_set_event("malformed oversize %lu", (unsigned long)data_len);
#endif
        uint32_t remaining = data_len;
        uint8_t  drain[32];
        while (remaining > 0) {
            uint32_t chunk = remaining < sizeof(drain) ? remaining : sizeof(drain);
            if (!tcp_read_n(c, drain, chunk)) return 0;
            remaining -= chunk;
        }
        return 0;
    }

    /* Store frag at start of buffer */
    put_u32(rx_buf, frag);

    /* Read the rest */
    if (data_len == 0) return 4;
    if (!tcp_read_n(c, rx_buf + 4, data_len)) return 0;

    return 4 + data_len;
}

/* ── Send portmapper GETPORT reply (TCP or UDP) ──────────────────────────── */
/*
 * UDP variant omits the 4-byte frag word; TCP includes it.
 */
static void send_getport_tcp(WiFiClient &c, uint32_t xid, uint32_t port)
{
    uint8_t tx[36];
    /* payload = 1 × uint32_t (port) = 4 bytes */
    build_reply_hdr(tx, xid, 4);
    put_u32(tx + RPC_REPLY_HDR_LEN, port);
    c.write(tx, sizeof(tx));
    s_stats.last_getport_reply = (uint16_t)port;
    if (port == 0) s_stats.getport_zero_reply_count++;
    net_set_event("tcp getport->%u", port);
    DBGF("RPC TCP GETPORT → %u\n", port);
}

static void send_getport_udp(WiFiUDP &u, uint32_t xid, uint32_t port)
{
    /* UDP: no frag word, reply header is 6 × uint32_t = 24 bytes */
    uint8_t tx[28];
    put_u32(tx +  0, xid);
    put_u32(tx +  4, RPC_REPLY);
    put_u32(tx +  8, MSG_ACCEPTED);
    put_u32(tx + 12, 0);   /* verif flavor */
    put_u32(tx + 16, 0);   /* verif len    */
    put_u32(tx + 20, SUCCESS_STAT);
    put_u32(tx + 24, port);
    u.write(tx, sizeof(tx));
    s_stats.last_getport_reply = (uint16_t)port;
    if (port == 0) s_stats.getport_zero_reply_count++;
    net_set_event("udp getport->%u", port);
    DBGF("RPC UDP GETPORT → %u\n", port);
}

/* ── VXI-11 CREATE_LINK response ─────────────────────────────────────────── */
static void send_create_link(WiFiClient &c, uint32_t xid)
{
    /* payload: errorCode(4) + linkId(4) + abortPort(4) + maxRecvSize(4) = 16 */
    uint8_t tx[RPC_REPLY_HDR_LEN + 16];
    build_reply_hdr(tx, xid, 16);
    put_u32(tx + RPC_REPLY_HDR_LEN +  0, 0);           /* errorCode = 0       */
    put_u32(tx + RPC_REPLY_HDR_LEN +  4, 1);           /* linkId = 1          */
    put_u32(tx + RPC_REPLY_HDR_LEN +  8, 0);           /* abortPort = 0       */
    put_u32(tx + RPC_REPLY_HDR_LEN + 12, VXI_MAX_RECV_SIZE);
    c.write(tx, sizeof(tx));
    s_stats.create_link_count++;
    s_last_control_ms = millis();
    DBG("VXI CREATE_LINK");
}

/* ── VXI-11 DEVICE_WRITE response ────────────────────────────────────────── */
static void send_device_write_ok(WiFiClient &c, uint32_t xid, uint32_t written)
{
    /* payload: errorCode(4) + size(4) = 8 */
    uint8_t tx[RPC_REPLY_HDR_LEN + 8];
    build_reply_hdr(tx, xid, 8);
    put_u32(tx + RPC_REPLY_HDR_LEN + 0, 0);        /* errorCode = 0 */
    put_u32(tx + RPC_REPLY_HDR_LEN + 4, written);  /* bytes consumed */
    c.write(tx, sizeof(tx));
    s_stats.device_write_count++;
    s_stats.last_write_len = written;
    s_last_control_ms = millis();
}

/* ── VXI-11 DEVICE_READ response ─────────────────────────────────────────── */
static void send_device_read(WiFiClient &c, uint32_t xid)
{
    char resp[RESP_BUF_SIZE];
    uint32_t declared_len = 0;
    ScpiReadMode read_mode = SCPI_READ_NONE;
    int  resp_len = scpi_read_response(resp, sizeof(resp), &declared_len, &read_mode);

    if (resp_len <= 0) {
        /* Nothing pending – return empty with END reason */
        resp[0] = '\0';
        resp_len = 0;
        declared_len = 0;
    }

    if (declared_len < (uint32_t)resp_len) declared_len = (uint32_t)resp_len;

    /* XDR string padding: round up to 4-byte boundary */
    uint32_t padded = (declared_len + 3u) & ~3u;

    /* payload: errorCode(4) + reason(4) + dataLen(4) + data(padded) */
    uint8_t tx[RPC_REPLY_HDR_LEN + 12 + 128]; /* 128 > max RESP_BUF_SIZE padded */
    if (padded > 128) padded = 128;            /* hard clamp – should never trigger */

    build_reply_hdr(tx, xid, 12 + padded);
    put_u32(tx + RPC_REPLY_HDR_LEN +  0, 0);           /* errorCode = 0      */
    put_u32(tx + RPC_REPLY_HDR_LEN +  4, 0x04u);       /* reason = END       */
    put_u32(tx + RPC_REPLY_HDR_LEN +  8, declared_len); /* declared len */

    memset(tx + RPC_REPLY_HDR_LEN + 12, 0, padded);
    if (resp_len > 0)
        memcpy(tx + RPC_REPLY_HDR_LEN + 12, resp, (size_t)resp_len);

    c.write(tx, RPC_REPLY_HDR_LEN + 12 + padded);
    s_stats.device_read_count++;
    s_stats.last_read_len = (uint32_t)resp_len;
    s_stats.last_read_declared_len = declared_len;
    s_stats.last_read_fixed_id = (read_mode == SCPI_READ_FIXED_ID);
    s_last_control_ms = millis();
    if (resp_len > 0) {
#if ENABLE_PROTOCOL_DIAG
        char preview[65];
        proto_diag_sanitize_preview((const uint8_t *)resp, (uint32_t)resp_len,
            preview, sizeof(preview));
        net_set_event("device_read %lu %s '%s'",
            (unsigned long)declared_len,
            (read_mode == SCPI_READ_FIXED_ID) ? "fixed_id" : "parser",
            preview);
#else
        net_set_event("device_read %lu", (unsigned long)declared_len);
#endif
    } else {
        net_set_event("device_read %d", resp_len);
    }
    DBGF("VXI DEVICE_READ → %d bytes\n", resp_len);
}

/* ── VXI-11 DESTROY_LINK response ────────────────────────────────────────── */
static void send_destroy_link(WiFiClient &c, uint32_t xid)
{
    /* payload: errorCode(4) = 4 bytes */
    uint8_t tx[RPC_REPLY_HDR_LEN + 4];
    build_reply_hdr(tx, xid, 4);
    put_u32(tx + RPC_REPLY_HDR_LEN, 0); /* errorCode = 0 */
    c.write(tx, sizeof(tx));
    s_stats.destroy_link_count++;
    s_last_control_ms = millis();
    net_set_event("destroy_link");
    DBG("VXI DESTROY_LINK");
}

/* ── Handle one complete RPC record from the VXI session client ─────────── */
/*
 * Returns:  0 = session continues
 *           1 = session should end (DESTROY_LINK or error)
 */
static int handle_vxi_record(WiFiClient &c, const uint8_t *rx, uint32_t rx_len)
{
    if (rx_len < (uint32_t)PAYLOAD_OFFSET) {
        s_stats.malformed_packet_count++;
        net_set_event("malformed vxi len=%lu", (unsigned long)rx_len);
        return 1;
    }

    uint32_t xid       = get_u32(rx + RPC_HDR_OFFSET + 0);
    uint32_t msg_type  = get_u32(rx + RPC_HDR_OFFSET + 4);
    uint32_t program   = get_u32(rx + RPC_HDR_OFFSET + 12);
    uint32_t version   = get_u32(rx + RPC_HDR_OFFSET + 16);
    uint32_t procedure = get_u32(rx + RPC_HDR_OFFSET + 20);

    if (msg_type != RPC_CALL) return 0;  /* ignore non-CALL */
    note_scope_activity();

#if ENABLE_PROTOCOL_DIAG
    proto_diag_record_vxi_call(program, version, procedure, xid);
#endif

    /* ── Portmapper on VXI port (shouldn't happen but handle gracefully) */
    if (program == PORTMAP_PROG) {
        if (rx_len < (uint32_t)(PAYLOAD_OFFSET + 16)) {
            s_stats.malformed_packet_count++;
            net_set_event("malformed vxi getport");
            return 0;
        }
        uint32_t map_prog  = get_u32(rx + PAYLOAD_OFFSET + 0);
        uint32_t map_vers  = get_u32(rx + PAYLOAD_OFFSET + 4);
        uint32_t map_proto = get_u32(rx + PAYLOAD_OFFSET + 8);
        uint32_t port = getport_reply_port(map_prog, map_vers, map_proto);
#if ENABLE_PROTOCOL_DIAG
        proto_diag_record_getport("TCP", map_prog, map_vers, map_proto, port);
#endif
        net_set_event("vxi portmap %08lX/%lu/%lu",
            (unsigned long)map_prog,
            (unsigned long)map_vers,
            (unsigned long)map_proto);
        if (port == 0) {
            net_set_event("zero-port reply %08lX/%lu/%lu",
                (unsigned long)map_prog,
                (unsigned long)map_vers,
                (unsigned long)map_proto);
        }
        send_getport_tcp(c, xid, port);
        return 0;
    }

    if (program != VXI11_CORE_PROG) {
        net_set_event("unexpected rpc %08lX/%lu/%lu",
            (unsigned long)program,
            (unsigned long)version,
            (unsigned long)procedure);
        return 0;
    }

    switch (procedure) {

    case VXI11_CREATE_LINK: {
#if ENABLE_PROTOCOL_DIAG
        uint32_t client_id = 0;
        uint32_t lock_device = 0;
        uint32_t lock_timeout = 0;
        char device[NET_PROTO_DEVICE_LEN];

        device[0] = '\0';
        if (proto_diag_parse_create_link(rx, rx_len, &client_id,
                &lock_device, &lock_timeout, device, sizeof(device))) {
            s_stats.last_create_link_valid = true;
            s_stats.last_create_client_id = client_id;
            s_stats.last_create_lock_device = (uint8_t)(lock_device ? 1u : 0u);
            s_stats.last_create_lock_timeout = lock_timeout;
            proto_diag_copy_string(s_stats.last_create_device,
                sizeof(s_stats.last_create_device), device);
            net_set_event("create_link dev=%s cid=%lu",
                device[0] ? device : "-",
                (unsigned long)client_id);
        } else {
            s_stats.malformed_packet_count++;
            s_stats.last_create_link_valid = false;
            s_stats.last_create_device[0] = '\0';
            net_set_event("create_link parse failed");
        }
#else
        net_set_event("create_link");
#endif
        send_create_link(c, xid);
        return 0;
    }

    case VXI11_DEVICE_WRITE: {
        /*
         * Payload layout after standard RPC header (offset 44 from frag):
         *   +0  lid          +4  ioTimeout   +8  lockTimeout
         *   +12 flags        +16 dataLen     +20 data[]
         */
        if (rx_len < (uint32_t)(PAYLOAD_OFFSET + 20)) break;
        uint32_t data_len = get_u32(rx + PAYLOAD_OFFSET + 16);
        uint32_t data_off = VXI_WRITE_DATA_OFFSET;

        if (data_len == 0 || data_off + data_len > rx_len) {
            if (data_len > 0 && data_off + data_len > rx_len) {
                s_stats.malformed_packet_count++;
                net_set_event("malformed write len=%lu", (unsigned long)data_len);
            } else {
                net_set_event("device_write %lu", (unsigned long)data_len);
            }
            send_device_write_ok(c, xid, 0);
            return 0;
        }

        /* Build a null-terminated copy of the SCPI command */
        static char cmd_buf[CMD_BUF_SIZE];
#if ENABLE_PROTOCOL_DIAG
        char preview[65];
#endif
        uint32_t clen = data_len < CMD_BUF_SIZE - 1 ? data_len : CMD_BUF_SIZE - 1;
        memcpy(cmd_buf, rx + data_off, clen);
        cmd_buf[clen] = '\0';

#if ENABLE_PROTOCOL_DIAG
        proto_diag_sanitize_preview(rx + data_off, data_len, preview, sizeof(preview));
        net_set_event("device_write %lu '%s'", (unsigned long)data_len, preview);
#else
        net_set_event("device_write %lu", (unsigned long)data_len);
#endif

    bool resync_for_setup = should_force_awg_resync_setup(cmd_buf);
    bool resync_for_idle = !resync_for_setup && should_force_awg_resync_idle();
    if (resync_for_setup || resync_for_idle) {
            fy_force_resync();
        net_set_event(resync_for_setup ? "awg resync before setup write"
                       : "awg resync after idle gap");
        }

        if (!scpi_handle_write(cmd_buf)) {
            net_set_event("ignored invalid scpi");
        }
        send_device_write_ok(c, xid, data_len);
        return 0;
    }

    case VXI11_DEVICE_READ:
        send_device_read(c, xid);
        return 0;

    case VXI11_DESTROY_LINK:
        send_destroy_link(c, xid);
        return 1;   /* signal session end */

    default:
        s_stats.unknown_proc_count++;
        net_set_event("unknown proc %lu", (unsigned long)procedure);
        DBGF("VXI: unknown procedure %u\n", procedure);
        return 0;
    }

    return 0;
}

/* ── UDP portmapper poll ─────────────────────────────────────────────────── */
static void poll_udp(void)
{
    int pkt_size = s_udp.parsePacket();
    if (pkt_size <= 0) return;

    uint8_t buf[80];
    int     n = s_udp.read(buf, sizeof(buf));
    if (n < 24) {
        s_stats.malformed_packet_count++;
        net_set_event("malformed udp len=%d", n);
        return;
    }

    uint32_t msg_type  = get_u32(buf + 4);
    uint32_t program   = get_u32(buf + 12);
    uint32_t prog_vers = get_u32(buf + 16);
    uint32_t procedure = get_u32(buf + 20);

    if (msg_type  != RPC_CALL)       return;
    if (program != PORTMAP_PROG || procedure != PORTMAP_GETPORT) {
        net_set_event("unexpected udp rpc %08lX/%lu/%lu",
            (unsigned long)program,
            (unsigned long)prog_vers,
            (unsigned long)procedure);
        return;
    }
    if (n < 56) {
        s_stats.malformed_packet_count++;
        net_set_event("malformed udp getport len=%d", n);
        return;
    }
    s_stats.udp_getport_count++;
    note_scope_activity();

    if (prog_vers != PORTMAP_VERS) {
        uint32_t xid = get_u32(buf + 0);
        uint32_t map_prog  = get_u32(buf + 40);
        uint32_t map_vers  = get_u32(buf + 44);
        uint32_t map_proto = get_u32(buf + 48);
#if ENABLE_PROTOCOL_DIAG
        proto_diag_record_getport("UDP", map_prog, map_vers, map_proto, 0);
#endif
        net_set_event("udp getport %08lX/%lu/%lu",
            (unsigned long)map_prog,
            (unsigned long)map_vers,
            (unsigned long)map_proto);
        net_set_event("zero-port reply %08lX/%lu/%lu",
            (unsigned long)map_prog,
            (unsigned long)map_vers,
            (unsigned long)map_proto);
        s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
        send_getport_udp(s_udp, xid, 0);
        s_udp.endPacket();
        return;
    }

    uint32_t xid = get_u32(buf + 0);
    uint32_t map_prog  = get_u32(buf + 40);
    uint32_t map_vers  = get_u32(buf + 44);
    uint32_t map_proto = get_u32(buf + 48);
    uint32_t port      = getport_reply_port(map_prog, map_vers, map_proto);

#if ENABLE_PROTOCOL_DIAG
    proto_diag_record_getport("UDP", map_prog, map_vers, map_proto, port);
#endif
    net_set_event("udp getport %08lX/%lu/%lu",
        (unsigned long)map_prog,
        (unsigned long)map_vers,
        (unsigned long)map_proto);
    if (port == 0) {
        net_set_event("zero-port reply %08lX/%lu/%lu",
            (unsigned long)map_prog,
            (unsigned long)map_vers,
            (unsigned long)map_proto);
    }

    s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
    send_getport_udp(s_udp, xid, port);
    s_udp.endPacket();
    DBGF("UDP portmap from %s:%u\n",
         s_udp.remoteIP().toString().c_str(), s_udp.remotePort());
}

/* ── TCP portmapper (port 111) poll ─────────────────────────────────────── */
static void poll_rpc_tcp(void)
{
    WiFiClient c = s_rpc_tcp.accept();
    if (!c) return;

    static uint8_t rx[RX_BUF_SIZE];
    uint32_t rx_len = read_rpc_tcp(c, rx);
    if (rx_len > 0 && rx_len < (uint32_t)(PAYLOAD_OFFSET + 16)) {
        s_stats.malformed_packet_count++;
        net_set_event("malformed tcp getport len=%lu", (unsigned long)rx_len);
    }
    if (rx_len >= (uint32_t)(PAYLOAD_OFFSET + 16)) {
        uint32_t xid       = get_u32(rx + RPC_HDR_OFFSET + 0);
        uint32_t msg_type  = get_u32(rx + RPC_HDR_OFFSET + 4);
        uint32_t program   = get_u32(rx + RPC_HDR_OFFSET + 12);
        uint32_t prog_vers = get_u32(rx + RPC_HDR_OFFSET + 16);
        uint32_t procedure = get_u32(rx + RPC_HDR_OFFSET + 20);

        if (msg_type == RPC_CALL && program == PORTMAP_PROG
                && procedure == PORTMAP_GETPORT) {
            s_stats.tcp_getport_count++;
            note_scope_activity();
            uint32_t port = 0;
            uint32_t map_prog  = get_u32(rx + PAYLOAD_OFFSET + 0);
            uint32_t map_vers  = get_u32(rx + PAYLOAD_OFFSET + 4);
            uint32_t map_proto = get_u32(rx + PAYLOAD_OFFSET + 8);
            if (prog_vers == PORTMAP_VERS) {
                port = getport_reply_port(map_prog, map_vers, map_proto);
            }
#if ENABLE_PROTOCOL_DIAG
            proto_diag_record_getport("TCP", map_prog, map_vers, map_proto, port);
#endif
            net_set_event("tcp getport %08lX/%lu/%lu",
                (unsigned long)map_prog,
                (unsigned long)map_vers,
                (unsigned long)map_proto);
            if (port == 0) {
                net_set_event("zero-port reply %08lX/%lu/%lu",
                    (unsigned long)map_prog,
                    (unsigned long)map_vers,
                    (unsigned long)map_proto);
            }
            send_getport_tcp(c, xid, port);
        } else if (msg_type == RPC_CALL) {
            net_set_event("unexpected tcp rpc %08lX/%lu/%lu",
                (unsigned long)program,
                (unsigned long)prog_vers,
                (unsigned long)procedure);
        }
    }
    /* Flush send buffer, give client time to read, then close */
    c.flush();
    delay(5);
    c.stop();
}

/* ── VXI session poll ────────────────────────────────────────────────────── */
static void poll_vxi(void)
{
    static uint8_t rx[RX_BUF_SIZE];

    /* Accept new connection if no active session */
    if (!s_session_active) {
        WiFiClient ca = s_vxi_a.accept();
        WiFiClient cb = s_vxi_b.accept();
        WiFiClient &cc = ca ? ca : cb;
        if (!cc) return;
        s_vxi_client   = cc;
        s_session_active = true;
        s_stats.session_accept_count++;
        s_stats.session_active = true;
        s_stats.active_client_ip = cc.remoteIP();
        s_stats.active_client_port = cc.remotePort();
        s_stats.current_vxi_port = s_cur_vxi_port;
        note_scope_activity();
        net_set_event("session accepted %s:%u",
            cc.remoteIP().toString().c_str(), cc.remotePort());
        DBGF("VXI session accepted on port %u\n", s_cur_vxi_port);
        return;
    }

    /* Check for dropped connection */
    if (!s_vxi_client.connected()) {
        DBGF("VXI session dropped\n");
        s_vxi_client.stop();
        s_session_active = false;
        s_stats.session_drop_count++;
        s_stats.session_active = false;
        s_stats.active_client_ip = IPAddress((uint32_t)0);
        s_stats.active_client_port = 0;
        net_set_event("session dropped");
        rotate_vxi_port();
        return;
    }

    /* Only process when data is available (non-blocking) */
    if (!s_vxi_client.available()) return;

    uint32_t rx_len = read_rpc_tcp(s_vxi_client, rx);
    if (rx_len == 0) {
        /* Read error / timeout */
        s_vxi_client.stop();
        s_session_active = false;
        s_stats.session_drop_count++;
        s_stats.session_active = false;
        s_stats.active_client_ip = IPAddress((uint32_t)0);
        s_stats.active_client_port = 0;
        net_set_event("session timeout");
        rotate_vxi_port();
        return;
    }

    int end_session = handle_vxi_record(s_vxi_client, rx, rx_len);
    if (end_session) {
        s_vxi_client.flush();
        delay(5);
        s_vxi_client.stop();
        s_session_active = false;
        s_stats.session_end_count++;
        s_stats.session_active = false;
        s_stats.active_client_ip = IPAddress((uint32_t)0);
        s_stats.active_client_port = 0;
        rotate_vxi_port();
        net_set_event("session closed");
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void net_begin(void)
{
    s_stats = NetStats();
    s_udp.begin(RPC_PORT);
    s_rpc_tcp.begin();
    s_vxi_a.begin();
    s_vxi_b.begin();

    s_cur_vxi_port   = VXI_PORT_A;
    s_session_active = false;
    s_last_control_ms = 0;
    s_last_scope_activity_ms = 0;
    s_scope_activity_seen = false;
    s_auto_output_off_done = false;
    s_stats.current_vxi_port = s_cur_vxi_port;
#if ENABLE_PROTOCOL_DIAG
    proto_diag_set_transport("-");
#endif
    net_set_event("net ready");

    DBG("Network servers started (UDP 111, TCP 111, VXI 9009/9010)");
}

void net_poll(void)
{
    poll_udp();
    poll_rpc_tcp();
    poll_vxi();
    handle_auto_output_off();
}

const NetStats *net_get_stats(void)
{
    s_stats.current_vxi_port = s_cur_vxi_port;
    s_stats.session_active = s_session_active;
    return &s_stats;
}

void net_clear_protocol_diag(void)
{
    IPAddress active_ip = s_stats.active_client_ip;
    uint16_t active_port = s_stats.active_client_port;

    s_stats = NetStats();
    s_stats.current_vxi_port = s_cur_vxi_port;
    s_stats.session_active = s_session_active;
    s_stats.active_client_ip = active_ip;
    s_stats.active_client_port = active_port;
#if ENABLE_PROTOCOL_DIAG
    proto_diag_set_transport("-");
#endif
    net_set_event("protocol diagnostics cleared");
}
