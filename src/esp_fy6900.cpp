#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "esp_fy6900.h"
#include "esp_persist.h"
#include "esp_config.h"
#include "esp_diag_console.h"

/* ── Global state cache ─────────────────────────────────────────────────── */
AwgState g_awg;
static AwgState  s_awg_sent;
static bool     s_awg_enabled = ENABLE_AWG_AUTOSTREAM ? true : false;
static uint32_t s_awg_baud    = AWG_BAUD_RATE;
static bool     s_awg_sent_valid = false;

static void fy_reset_cache(void)
{
    g_awg.ch1_on        = 0;
    g_awg.ch1_wave      = AWG_SINE;
    g_awg.ch1_freq_hz   = 1000.0;
    g_awg.ch1_ampl_v    = 1.0;
    g_awg.ch1_ofst_v    = 0.0;
    g_awg.ch1_drive_ampl_v = 2.0;
    g_awg.ch1_drive_ofst_v = 0.0;
    g_awg.ch1_phase_deg = 0.0;
    g_awg.ch1_load      = AWG_LOAD_50;

    g_awg.ch2_on        = 0;
    g_awg.ch2_wave      = AWG_SINE;
    g_awg.ch2_freq_hz   = 1000.0;
    g_awg.ch2_ampl_v    = 1.0;
    g_awg.ch2_ofst_v    = 0.0;
    g_awg.ch2_drive_ampl_v = 2.0;
    g_awg.ch2_drive_ofst_v = 0.0;
    g_awg.ch2_phase_deg = 0.0;
    g_awg.ch2_load      = AWG_LOAD_50;
}

static void fy_reset_sent_state(void)
{
    memset(&s_awg_sent, 0, sizeof(s_awg_sent));
    s_awg_sent_valid = false;
}

static bool same_value(double left, double right, double epsilon)
{
    double delta = left - right;
    if (delta < 0.0) delta = -delta;
    return delta < epsilon;
}

static double fy_load_coeff(AwgLoadMode load)
{
    switch (load) {
    case AWG_LOAD_50:
        return 50.0 / (50.0 + 50.0);
    case AWG_LOAD_75:
        return 75.0 / (75.0 + 50.0);
    case AWG_LOAD_HIZ:
    default:
        return 1.0;
    }
}

static void fy_update_drive_values(uint8_t ch)
{
    double coeff;

    if (ch == 1) {
        coeff = fy_load_coeff(g_awg.ch1_load);
        g_awg.ch1_drive_ampl_v = g_awg.ch1_ampl_v / coeff;
        g_awg.ch1_drive_ofst_v = g_awg.ch1_ofst_v / coeff;
    } else {
        coeff = fy_load_coeff(g_awg.ch2_load);
        g_awg.ch2_drive_ampl_v = g_awg.ch2_ampl_v / coeff;
        g_awg.ch2_drive_ofst_v = g_awg.ch2_ofst_v / coeff;
    }
}

static void awg_trace_tx(const char *buf, size_t len)
{
    diag_tracef("[awg tx] %.*s", (int)len, buf);
}

static bool is_valid_generator_rx_byte(uint8_t value)
{
    return value == '\r' || value == '\n' || value == '\t' || (value >= 32u && value <= 126u);
}

static int read_filtered_serial(char *buf, int buf_max, bool *saw_invalid)
{
    int n = 0;

    if (saw_invalid) *saw_invalid = false;
    if (buf_max <= 0) return 0;

    while (Serial.available()) {
        uint8_t value = (uint8_t)Serial.read();
        if (!is_valid_generator_rx_byte(value)) {
            if (saw_invalid) *saw_invalid = true;
            continue;
        }
        if (n < buf_max - 1) {
            buf[n++] = (char)value;
        }
    }

    buf[n] = '\0';
    return n;
}

/*
 * FY6900 serial protocol (later firmware, Hz format)
 * ──────────────────────────────────────────────────
 * Set commands: "W<ch><code><value>\n"
 *   ch   : M (ch1) | F (ch2)
 *   codes: N=output  W=wave  F=freq  A=ampl  O=offset  P=phase
 *
 * Frequency format (CRITICAL – later FY6900 requires Hz with decimal):
 *   %015.6f  →  e.g., "000001000.000000" for 1000 Hz
 *   Older FY6800 / early FY6900 used uHz without decimal:
 *   %08lu000000 → e.g., "00001000000000"
 *
 * All other parameters use straightforward decimal with units:
 *   Amplitude  : "%.4f"  Volts  e.g. "2.0000"
 *   Offset     : "%.3f"  Volts  e.g. "-0.500" (signed)
 *   Phase      : "%.3f"  degrees (signed)
 *   Wave type  : "%02u"  integer index
 *   Output     : "0" or "1"
 *
 * After every set command the AWG echoes a single "\n".
 * wait_ack() drains that echo (or times out gracefully).
 */

static inline char ch_letter(uint8_t ch)
{
    return (ch == 1) ? 'M' : 'F';
}

bool fy_is_supported_baud(uint32_t baud)
{
    switch (baud) {
        case 9600u:
        case 19200u:
        case 38400u:
        case 57600u:
        case 115200u:
            return true;
        default:
            return false;
    }
}

/* Drain the AWG's "\n" echo after a set command */
static void wait_ack(void)
{
    uint32_t t0 = millis();
    bool saw_invalid = false;
    char ack_buf[8];

    while (!Serial.available()) {
        if ((millis() - t0) > AWG_SERIAL_TIMEOUT_MS) return;
        yield();
    }

    read_filtered_serial(ack_buf, sizeof(ack_buf), &saw_invalid);
    if (saw_invalid) {
        diag_tracef("[awg rx] ignored invalid ttl bytes\r\n");
    }
}

/* Send a command string and wait for the AWG echo */
static void send_cmd(const char *buf, size_t len)
{
    if (!s_awg_enabled) return;
    awg_trace_tx(buf, len);
    Serial.write((const uint8_t *)buf, len);
    wait_ack();
}

static void fy_program_channel(uint8_t ch)
{
    char buf[28];
    int  n;
    uint8_t on = (ch == 1) ? g_awg.ch1_on : g_awg.ch2_on;
    AwgWaveType wave = (ch == 1) ? g_awg.ch1_wave : g_awg.ch2_wave;
    double freq = (ch == 1) ? g_awg.ch1_freq_hz : g_awg.ch2_freq_hz;
    double drive_ampl = (ch == 1) ? g_awg.ch1_drive_ampl_v : g_awg.ch2_drive_ampl_v;
    double drive_ofst = (ch == 1) ? g_awg.ch1_drive_ofst_v : g_awg.ch2_drive_ofst_v;

    n = snprintf(buf, sizeof(buf), "W%cN%c\n", ch_letter(ch), on ? '1' : '0');
    if (n > 0 && n < (int)sizeof(buf)) send_cmd(buf, (size_t)n);

    n = snprintf(buf, sizeof(buf), "W%cW%02u\n", ch_letter(ch), (unsigned)wave);
    if (n > 0 && n < (int)sizeof(buf)) send_cmd(buf, (size_t)n);

    n = snprintf(buf, sizeof(buf), "W%cF%015.6f\n", ch_letter(ch), freq);
    if (n > 0 && n < (int)sizeof(buf)) send_cmd(buf, (size_t)n);

    n = snprintf(buf, sizeof(buf), "W%cA%.4f\n", ch_letter(ch), drive_ampl);
    if (n > 0 && n < (int)sizeof(buf)) send_cmd(buf, (size_t)n);

    n = snprintf(buf, sizeof(buf), "W%cO%.3f\n", ch_letter(ch), drive_ofst);
    if (n > 0 && n < (int)sizeof(buf)) send_cmd(buf, (size_t)n);

    DBGF("AWG resync ch%u on=%u load=%s req=%.4f drive=%.4f ofst=%.3f/%.3f\n",
        ch,
        (unsigned)on,
        fy_load_to_text((ch == 1) ? g_awg.ch1_load : g_awg.ch2_load),
        (ch == 1) ? g_awg.ch1_ampl_v : g_awg.ch2_ampl_v,
        drive_ampl,
        (ch == 1) ? g_awg.ch1_ofst_v : g_awg.ch2_ofst_v,
        drive_ofst);
}

static void fy_resync(void)
{
    char buf[16];
    int  n;

    if (!s_awg_enabled) return;

    while (Serial.available()) Serial.read();

    fy_update_drive_values(1);
    fy_update_drive_values(2);

    DBGF("AWG resync start\n");

    fy_program_channel(1);
    fy_program_channel(2);

    n = snprintf(buf, sizeof(buf), "WFP%.3f\n", g_awg.ch1_phase_deg);
    if (n > 0 && n < (int)sizeof(buf)) send_cmd(buf, (size_t)n);
    DBGF("AWG resync phase=%.3f\n", g_awg.ch1_phase_deg);

    s_awg_sent = g_awg;
    s_awg_sent_valid = true;
}

/* ── Init ────────────────────────────────────────────────────────────────── */
void fy_init(void)
{
    s_awg_baud = fy_is_supported_baud(g_config.awg_baud) ? g_config.awg_baud : AWG_BAUD_RATE;
    Serial.begin(s_awg_baud);
    Serial.setTimeout(AWG_SERIAL_TIMEOUT_MS);

    /* Flush any stale bytes */
    delay(50);
    while (Serial.available()) Serial.read();

    fy_reset_cache();
    fy_reset_sent_state();

    if (s_awg_enabled) {
        fy_resync();
    }
}

bool fy_is_enabled(void)
{
    return s_awg_enabled;
}

void fy_set_enabled(bool enabled)
{
    bool was_enabled = s_awg_enabled;
    s_awg_enabled = enabled;
    if (!was_enabled && enabled) {
        fy_reset_sent_state();
        fy_resync();
    }
}

uint32_t fy_get_baud(void)
{
    return s_awg_baud;
}

void fy_set_baud(uint32_t baud)
{
    if (!fy_is_supported_baud(baud)) return;
    s_awg_baud = baud;
    Serial.flush();
    Serial.begin(s_awg_baud);
    Serial.setTimeout(AWG_SERIAL_TIMEOUT_MS);
}

void fy_force_resync(void)
{
    fy_resync();
}

void fy_restore_startup_state(void)
{
    fy_reset_cache();
    fy_reset_sent_state();
    if (s_awg_enabled) {
        fy_resync();
    }
}

int fy_read_available(char *buf, int buf_max)
{
    bool saw_invalid = false;
    int n = read_filtered_serial(buf, buf_max, &saw_invalid);
    if (saw_invalid) {
        diag_tracef("[awg rx] ignored invalid ttl bytes\r\n");
    }
    return n;
}

/* ── Output on/off ──────────────────────────────────────────────────────── */
void fy_set_output(uint8_t ch, uint8_t on)
{
    char buf[8];
    if (ch == 1) g_awg.ch1_on = on;
    else         g_awg.ch2_on = on;

    if (s_awg_sent_valid) {
        uint8_t sent = (ch == 1) ? s_awg_sent.ch1_on : s_awg_sent.ch2_on;
        if (sent == on) return;
    }

    int  n = snprintf(buf, sizeof(buf), "W%cN%c\n", ch_letter(ch), on ? '1' : '0');
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    send_cmd(buf, (size_t)n);
    if (ch == 1) s_awg_sent.ch1_on = on;
    else         s_awg_sent.ch2_on = on;
    s_awg_sent_valid = true;
    DBGF("AWG ch%u output %s\n", ch, on ? "ON" : "OFF");
}

/* ── Wave type ──────────────────────────────────────────────────────────── */
void fy_set_wave(uint8_t ch, AwgWaveType wave)
{
    char buf[10];
    if (ch == 1) g_awg.ch1_wave = wave;
    else         g_awg.ch2_wave = wave;

    if (s_awg_sent_valid) {
        AwgWaveType sent = (ch == 1) ? s_awg_sent.ch1_wave : s_awg_sent.ch2_wave;
        if (sent == wave) return;
    }

    int  n = snprintf(buf, sizeof(buf), "W%cW%02u\n", ch_letter(ch), (unsigned)wave);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    send_cmd(buf, (size_t)n);
    if (ch == 1) s_awg_sent.ch1_wave = wave;
    else         s_awg_sent.ch2_wave = wave;
    s_awg_sent_valid = true;
    DBGF("AWG ch%u wave %u\n", ch, (unsigned)wave);
}

/* ── Frequency (Hz, with decimal point – later FY6900 format) ───────────── */
/*
 * Format: WMF000001000.000000\n
 *         ↑ W + channel_letter + F + %015.6f + \n
 * %015.6f with 1000.0 → "000001000.000000" (15 total chars before and incl. dot)
 * This matches hb020's fy6900.py: "%015.6f" % freq
 */
void fy_set_freq(uint8_t ch, double freq_hz)
{
    char buf[28];
    if (ch == 1) g_awg.ch1_freq_hz = freq_hz;
    else         g_awg.ch2_freq_hz = freq_hz;

    if (s_awg_sent_valid) {
        double sent = (ch == 1) ? s_awg_sent.ch1_freq_hz : s_awg_sent.ch2_freq_hz;
        if (same_value(sent, freq_hz, 0.0000005)) return;
    }

    /* WMF + 15 chars (8 int digits + dot + 6 decimal) + \n = 19 chars total. */
    int n = snprintf(buf, sizeof(buf), "W%cF%015.6f\n", ch_letter(ch), freq_hz);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    send_cmd(buf, (size_t)n);
    if (ch == 1) s_awg_sent.ch1_freq_hz = freq_hz;
    else         s_awg_sent.ch2_freq_hz = freq_hz;
    s_awg_sent_valid = true;
    DBGF("AWG ch%u freq %.6f Hz\n", ch, freq_hz);
}

/* ── Amplitude (Vpp) ────────────────────────────────────────────────────── */
void fy_set_ampl(uint8_t ch, double ampl_v)
{
    char buf[16];
    if (ch == 1) g_awg.ch1_ampl_v = ampl_v;
    else         g_awg.ch2_ampl_v = ampl_v;

    fy_update_drive_values(ch);

    if (s_awg_sent_valid) {
        double sent = (ch == 1) ? s_awg_sent.ch1_drive_ampl_v : s_awg_sent.ch2_drive_ampl_v;
        double drive = (ch == 1) ? g_awg.ch1_drive_ampl_v : g_awg.ch2_drive_ampl_v;
        if (same_value(sent, drive, 0.00005)) return;
    }

    double drive = (ch == 1) ? g_awg.ch1_drive_ampl_v : g_awg.ch2_drive_ampl_v;
    int  n = snprintf(buf, sizeof(buf), "W%cA%.4f\n", ch_letter(ch), drive);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    send_cmd(buf, (size_t)n);
    if (ch == 1) {
        s_awg_sent.ch1_ampl_v = ampl_v;
        s_awg_sent.ch1_drive_ampl_v = drive;
    } else {
        s_awg_sent.ch2_ampl_v = ampl_v;
        s_awg_sent.ch2_drive_ampl_v = drive;
    }
    s_awg_sent_valid = true;
    DBGF("AWG ch%u ampl req=%.4f drive=%.4f V\n", ch, ampl_v, drive);
}

/* ── Offset (V, signed) ──────────────────────────────────────────────────── */
void fy_set_offset(uint8_t ch, double ofst_v)
{
    char buf[16];
    if (ch == 1) g_awg.ch1_ofst_v = ofst_v;
    else         g_awg.ch2_ofst_v = ofst_v;

    fy_update_drive_values(ch);

    if (s_awg_sent_valid) {
        double sent = (ch == 1) ? s_awg_sent.ch1_drive_ofst_v : s_awg_sent.ch2_drive_ofst_v;
        double drive = (ch == 1) ? g_awg.ch1_drive_ofst_v : g_awg.ch2_drive_ofst_v;
        if (same_value(sent, drive, 0.0005)) return;
    }

    double drive = (ch == 1) ? g_awg.ch1_drive_ofst_v : g_awg.ch2_drive_ofst_v;
    int  n = snprintf(buf, sizeof(buf), "W%cO%.3f\n", ch_letter(ch), drive);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    send_cmd(buf, (size_t)n);
    if (ch == 1) {
        s_awg_sent.ch1_ofst_v = ofst_v;
        s_awg_sent.ch1_drive_ofst_v = drive;
    } else {
        s_awg_sent.ch2_ofst_v = ofst_v;
        s_awg_sent.ch2_drive_ofst_v = drive;
    }
    s_awg_sent_valid = true;
    DBGF("AWG ch%u offset req=%.3f drive=%.3f V\n", ch, ofst_v, drive);
}

/* ── Phase (degrees, signed) ─────────────────────────────────────────────── */
void fy_set_phase(uint8_t ch, double phase_deg)
{
    char buf[16];
    if (ch == 1) g_awg.ch1_phase_deg = phase_deg;
    else         g_awg.ch2_phase_deg = phase_deg;

    if (s_awg_sent_valid) {
        double sent = (ch == 1) ? s_awg_sent.ch1_phase_deg : s_awg_sent.ch2_phase_deg;
        if (same_value(sent, phase_deg, 0.0005)) return;
    }

    /* Phase must always be sent to channel 2 on FY-series (hardware quirk):
     * "WFP<value>\n" sets the relative phase of ch2 against ch1.
     * We cache the requested value for ch1 queries but always write to ch2.
     * Ref: hb020/sds1004x_bode fy.py set_phase() comment. */
    int n = snprintf(buf, sizeof(buf), "WFP%.3f\n", phase_deg);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    send_cmd(buf, (size_t)n);
    if (ch == 1) s_awg_sent.ch1_phase_deg = phase_deg;
    else         s_awg_sent.ch2_phase_deg = phase_deg;
    s_awg_sent_valid = true;
    DBGF("AWG phase %.3f deg\n", phase_deg);
}

void fy_set_load(uint8_t ch, AwgLoadMode load)
{
    AwgLoadMode current = (ch == 1) ? g_awg.ch1_load : g_awg.ch2_load;

    if (current == load) return;
    if (ch == 1) g_awg.ch1_load = load;
    else         g_awg.ch2_load = load;

    fy_update_drive_values(ch);
    if (ch == 1) s_awg_sent.ch1_load = load;
    else         s_awg_sent.ch2_load = load;

    /* Resend compensated output settings so the physical voltage matches the requested load. */
    fy_set_ampl(ch, (ch == 1) ? g_awg.ch1_ampl_v : g_awg.ch2_ampl_v);
    fy_set_offset(ch, (ch == 1) ? g_awg.ch1_ofst_v : g_awg.ch2_ofst_v);
    DBGF("AWG ch%u load %s\n", ch, fy_load_to_text(load));
}

AwgLoadMode fy_get_load(uint8_t ch)
{
    return (ch == 1) ? g_awg.ch1_load : g_awg.ch2_load;
}

double fy_get_drive_ampl(uint8_t ch)
{
    return (ch == 1) ? g_awg.ch1_drive_ampl_v : g_awg.ch2_drive_ampl_v;
}

double fy_get_drive_offset(uint8_t ch)
{
    return (ch == 1) ? g_awg.ch1_drive_ofst_v : g_awg.ch2_drive_ofst_v;
}

const char *fy_load_to_text(AwgLoadMode load)
{
    switch (load) {
    case AWG_LOAD_50: return "50";
    case AWG_LOAD_75: return "75";
    case AWG_LOAD_HIZ: return "HZ";
    default: return "?";
    }
}

/* ── Wave name translation ───────────────────────────────────────────────── */
static const struct {
    const char  *siglent;
    AwgWaveType  fy;
} s_wave_map[] = {
    { "SINE",     AWG_SINE      },
    { "SQUARE",   AWG_SQUARE    },
    { "RAMP",     AWG_POSRAMP   },
    { "TRIANGLE", AWG_TRIANGLE  },
    { "NOISE",    AWG_NOISE     },
    { NULL,       AWG_SINE      }  /* sentinel / default */
};

AwgWaveType fy_wave_from_siglent(const char *name)
{
    for (int i = 0; s_wave_map[i].siglent != NULL; i++) {
        if (strcasecmp(name, s_wave_map[i].siglent) == 0)
            return s_wave_map[i].fy;
    }
    return AWG_SINE;  /* default */
}

const char *fy_wave_to_siglent(AwgWaveType w)
{
    for (int i = 0; s_wave_map[i].siglent != NULL; i++) {
        if (s_wave_map[i].fy == w)
            return s_wave_map[i].siglent;
    }
    return "SINE";
}

/* ── Diagnostic raw command (debug builds only) ─────────────────────────── */
int fy_raw_cmd(const char *cmd, char *reply_buf, int reply_max)
{
    bool saw_invalid = false;

    if (!s_awg_enabled) return -1;
    /* Flush input */
    while (Serial.available()) Serial.read();
    awg_trace_tx(cmd, strlen(cmd));
    diag_tracef("\r\n");
    Serial.print(cmd);
    Serial.print('\n');
    delay(200);
    int n = read_filtered_serial(reply_buf, reply_max, &saw_invalid);
    if (saw_invalid) {
        diag_tracef("[awg rx] ignored invalid ttl bytes\r\n");
    }
    return n;
}
