#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_parser.h"
#include "esp_fy6900.h"
#include "esp_persist.h"
#include "esp_config.h"

/* ── Response buffer ─────────────────────────────────────────────────────── */
static char s_resp_buf[RESP_BUF_SIZE];
static int  s_resp_len = 0;
static uint32_t s_resp_declared_len = 0;
static ScpiReadMode s_resp_mode = SCPI_READ_NONE;

static const char *match_prefix(const char *src, const char *prefix);
static const char *skip_ws(const char *p);

typedef struct {
    bool has_wave;
    bool has_freq;
    bool has_ampl;
    bool has_offset;
    bool has_phase;
    AwgWaveType wave;
    double freq_hz;
    double ampl_v;
    double offset_v;
    double phase_deg;
} BswvPending;

static bool is_cmd_end(const char *p)
{
    p = skip_ws(p);
    return *p == '\0';
}

static bool match_exact_command(const char *src, const char *prefix)
{
    const char *rest = match_prefix(src, prefix);
    return rest != NULL && is_cmd_end(rest);
}

static bool is_valid_scpi_char(char ch)
{
    unsigned char value = (unsigned char)ch;
    return value >= 32u && value <= 126u;
}

static bool is_valid_scpi_buffer(const char *buf)
{
    bool has_visible_char = false;

    if (buf == NULL) return false;

    while (*buf) {
        char ch = *buf++;
        if (ch == '\r' || ch == '\n' || ch == '\t') continue;
        if (!is_valid_scpi_char(ch)) return false;
        if (ch != ' ') has_visible_char = true;
    }
    return has_visible_char;
}

/* ── Helper: parse a floating-point token from p.
 *   Requires the token to be syntactically valid and to end at ',', ';',
 *   whitespace, or string end. */
static bool parse_float_token(const char *p, const char **end_pp, double *value_out)
{
    double sign = 1.0;
    double val = 0.0;
    double frac = 1.0;
    bool   has_int_digits = false;
    bool   has_frac_digits = false;

    if (p == NULL || value_out == NULL) return false;
    p = skip_ws(p);

    if (*p == '-') { sign = -1.0; p++; }
    else if (*p == '+') { p++; }

    while (*p >= '0' && *p <= '9') {
        has_int_digits = true;
        val = val * 10.0 + (*p - '0');
        p++;
    }

    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            has_frac_digits = true;
            frac /= 10.0;
            val += frac * (*p - '0');
            p++;
        }
    }

    if (!has_int_digits && !has_frac_digits) return false;

    p = skip_ws(p);
    if (*p != '\0' && *p != ',' && *p != ';') return false;

    if (end_pp) *end_pp = p;
    *value_out = sign * val;
    return true;
}

/* ── Helper: case-insensitive prefix match.
 *   Returns pointer past the prefix in src if matched, else NULL. */
static const char *match_prefix(const char *src, const char *prefix)
{
    while (*prefix) {
        if (toupper((unsigned char)*src) != toupper((unsigned char)*prefix))
            return NULL;
        src++; prefix++;
    }
    return src;  /* points past the matched prefix */
}

/* ── Helper: skip optional whitespace ───────────────────────────────────── */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool parse_wave_token(const char *p, const char **end_pp, AwgWaveType *wave_out)
{
    char wave_name[10];
    int index = 0;

    if (p == NULL || wave_out == NULL) return false;
    p = skip_ws(p);
    while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') {
        if (index >= (int)sizeof(wave_name) - 1) return false;
        wave_name[index++] = (char)toupper((unsigned char)*p++);
    }
    wave_name[index] = '\0';
    p = skip_ws(p);

    if (index == 0) return false;
    if (*p != '\0' && *p != ',' && *p != ';') return false;

    if (strcmp(wave_name, "SINE") == 0) *wave_out = AWG_SINE;
    else if (strcmp(wave_name, "SQUARE") == 0) *wave_out = AWG_SQUARE;
    else if (strcmp(wave_name, "RAMP") == 0) *wave_out = AWG_POSRAMP;
    else if (strcmp(wave_name, "TRIANGLE") == 0) *wave_out = AWG_TRIANGLE;
    else if (strcmp(wave_name, "NOISE") == 0) *wave_out = AWG_NOISE;
    else return false;

    if (end_pp) *end_pp = p;
    return true;
}

static bool parse_load_mode(const char *text, const char **end_pp, AwgLoadMode *mode)
{
    char token[8];
    int index = 0;

    if (text == NULL || mode == NULL) return false;
    text = skip_ws(text);
    while (*text && *text != ',' && *text != ';' && *text != ' ' && *text != '\t') {
        if (index >= (int)sizeof(token) - 1) return false;
        token[index++] = (char)toupper((unsigned char)*text++);
    }
    token[index] = '\0';
    text = skip_ws(text);

    if (index == 0) return false;
    if (*text != '\0' && *text != ',' && *text != ';') return false;

    if (strcmp(token, "50") == 0) *mode = AWG_LOAD_50;
    else if (strcmp(token, "75") == 0) *mode = AWG_LOAD_75;
    else if (strcmp(token, "HZ") == 0 || strcmp(token, "HI-Z") == 0 || strcmp(token, "HIGHZ") == 0) *mode = AWG_LOAD_HIZ;
    else return false;

    if (end_pp) *end_pp = text;
    return true;
}

static bool is_valid_frequency(double value)
{
    return value > 0.0 && value <= 60000000.0;
}

static bool is_valid_amplitude(double value)
{
    return value >= 0.0 && value <= 20.0;
}

static bool is_valid_offset(double value)
{
    return value >= -10.0 && value <= 10.0;
}

static bool is_valid_phase(double value)
{
    return value >= -360.0 && value <= 360.0;
}

static char *trim_segment(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;

    char *end = s + strlen(s);
    while (end > s) {
        char ch = *(end - 1);
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        --end;
    }
    *end = '\0';
    return s;
}

static void set_text_response(const char *text, ScpiReadMode mode, uint32_t declared_len)
{
    s_resp_len = snprintf(s_resp_buf, sizeof(s_resp_buf), "%s", text);
    if (s_resp_len >= (int)sizeof(s_resp_buf)) {
        s_resp_len = (int)sizeof(s_resp_buf) - 1;
        s_resp_buf[s_resp_len] = '\0';
    }
    s_resp_declared_len = declared_len;
    s_resp_mode = mode;
}

static void set_idn_response(void)
{
    char response[RESP_BUF_SIZE];
    int response_len = snprintf(response, sizeof(response), "%s%s\n",
        IDN_RESPONSE_PREFIX, g_config.idn_response_name);

    if (response_len < 0) {
        s_resp_len = 0;
        s_resp_declared_len = 0;
        s_resp_mode = SCPI_READ_NONE;
        s_resp_buf[0] = '\0';
        return;
    }

    if (response_len >= (int)sizeof(response)) {
        response_len = (int)sizeof(response) - 1;
        response[response_len] = '\0';
    }

    set_text_response(response, SCPI_READ_FIXED_ID, (uint32_t)response_len);
}

static void format_bswv_response(uint8_t ch)
{
    char response[RESP_BUF_SIZE];
    double freq  = (ch == 1) ? g_awg.ch1_freq_hz   : g_awg.ch2_freq_hz;
    double ampl  = (ch == 1) ? g_awg.ch1_ampl_v    : g_awg.ch2_ampl_v;
    double ofst  = (ch == 1) ? g_awg.ch1_ofst_v    : g_awg.ch2_ofst_v;
    double phase = (ch == 1) ? g_awg.ch1_phase_deg : g_awg.ch2_phase_deg;
    AwgWaveType wt = (ch == 1) ? g_awg.ch1_wave    : g_awg.ch2_wave;
    int response_len = snprintf(response, sizeof(response),
        "C%u:BSWV WVTP,%s,FRQ,%.6fHz,AMP,%.6fV,OFST,%.6fV,PHSE,%.6fDeg\n",
        ch, fy_wave_to_siglent(wt), freq, ampl, ofst, phase);

    if (response_len < 0) {
        s_resp_len = 0;
        s_resp_declared_len = 0;
        s_resp_mode = SCPI_READ_NONE;
        s_resp_buf[0] = '\0';
        return;
    }

    if (response_len >= (int)sizeof(response)) {
        response_len = (int)sizeof(response) - 1;
        response[response_len] = '\0';
    }

    set_text_response(response, SCPI_READ_TEXT, (uint32_t)response_len);
}

/* ── Parse a BSWV parameter list "WVTP,SINE,FRQ,1000,AMP,2,OFST,0,PHSE,0"
 *   Modifies channel setters as it finds key=value pairs.                  */
static bool parse_bswv_params(uint8_t ch, const char *p)
{
    BswvPending pending = {};
    char key[8];
    int  ki;
    bool saw_param = false;

    p = skip_ws(p);
    if (*p == '\0') return false;

    while (p && *p) {
        /* extract key (up to comma or end) */
        ki = 0;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t' && ki < 7) {
            key[ki++] = (char)toupper((unsigned char)*p++);
        }
        key[ki] = '\0';
        p = skip_ws(p);
        if (ki == 0 || *p != ',') return false;
        p = skip_ws(p + 1);  /* skip comma between key and value */

        if (strcmp(key, "WVTP") == 0) {
            if (!parse_wave_token(p, &p, &pending.wave)) return false;
            pending.has_wave = true;

        } else if (strcmp(key, "FRQ") == 0) {
            if (!parse_float_token(p, &p, &pending.freq_hz) || !is_valid_frequency(pending.freq_hz)) return false;
            pending.has_freq = true;

        } else if (strcmp(key, "AMP") == 0) {
            if (!parse_float_token(p, &p, &pending.ampl_v) || !is_valid_amplitude(pending.ampl_v)) return false;
            pending.has_ampl = true;

        } else if (strcmp(key, "OFST") == 0) {
            if (!parse_float_token(p, &p, &pending.offset_v) || !is_valid_offset(pending.offset_v)) return false;
            pending.has_offset = true;

        } else if (strcmp(key, "PHSE") == 0) {
            if (!parse_float_token(p, &p, &pending.phase_deg) || !is_valid_phase(pending.phase_deg)) return false;
            pending.has_phase = true;

        } else {
            return false;
        }

        saw_param = true;
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        if (*p == '\0' || *p == ';') break;
        return false;
    }

    if (!saw_param) return false;

    if (pending.has_wave) fy_set_wave(ch, pending.wave);
    if (pending.has_freq) fy_set_freq(ch, pending.freq_hz);
    if (pending.has_ampl) fy_set_ampl(ch, pending.ampl_v);
    if (pending.has_offset) fy_set_offset(ch, pending.offset_v);
    if (pending.has_phase) fy_set_phase(ch, pending.phase_deg);
    return true;
}

/* ── Handle one SCPI sub-command (one token between semicolons).
 *   ch = channel context determined upstream (1 or 2).               */
static bool handle_one_cmd(uint8_t ch, const char *cmd)
{
    const char *rest;

    if (match_exact_command(cmd, "*RST") || match_exact_command(cmd, "SYST:PRESET")) {
        fy_restore_startup_state();
        DBGF("SCPI: reset startup state\n");
        return true;
    }

    /* IDN-SGLT-PRI? – no channel prefix needed */
    if (match_exact_command(cmd, "IDN-SGLT-PRI?")) {
        set_idn_response();
        DBG("SCPI: IDN query");
        return true;
    }

    /* C1: / C2: channel prefix */
    if ((rest = match_prefix(cmd, "C1:")) != NULL) { ch = 1; cmd = rest; }
    else if ((rest = match_prefix(cmd, "C2:")) != NULL) { ch = 2; cmd = rest; }

    /* BSWV? – query current waveform state */
    if (match_exact_command(cmd, "BSWV?")) {
        format_bswv_response(ch);
        DBGF("SCPI: BSWV? ch%u\n", ch);
        return true;
    }

    /* BSWV <params> – set waveform parameters */
    if ((rest = match_prefix(cmd, "BSWV ")) != NULL ||
        (rest = match_prefix(cmd, "BSWV\t")) != NULL) {
        rest = skip_ws(rest);
        if (!parse_bswv_params(ch, rest)) {
            DBGF("SCPI: invalid BSWV ch%u '%s'\n", ch, rest);
            return false;
        }
        DBGF("SCPI: BSWV set ch%u\n", ch);
        return true;
    }

    /* OUTP ON */
    if (match_exact_command(cmd, "OUTP ON")) {
        fy_set_output(ch, 1);
        DBGF("SCPI: OUTP ON ch%u\n", ch);
        return true;
    }

    /* OUTP OFF */
    if (match_exact_command(cmd, "OUTP OFF")) {
        fy_set_output(ch, 0);
        DBGF("SCPI: OUTP OFF ch%u\n", ch);
        return true;
    }

    /* OUTP LOAD,xx – accepted, not forwarded to FY (load setting) */
    if ((rest = match_prefix(cmd, "OUTP LOAD,")) != NULL) {
        AwgLoadMode load;
        const char *end = NULL;
        if (parse_load_mode(rest, &end, &load) && is_cmd_end(end)) {
            fy_set_load(ch, load);
            DBGF("SCPI: OUTP LOAD %s ch%u\n", fy_load_to_text(load), ch);
            return true;
        } else {
            DBGF("SCPI: OUTP LOAD invalid ch%u '%s'\n", ch, rest);
            return false;
        }
    }

    /* Unknown – log and ignore */
    DBGF("SCPI: unknown cmd '%s'\n", cmd);
    return false;
}

/* ── Public: handle a DEVICE_WRITE payload ──────────────────────────────── */
bool scpi_handle_write(char *buf)
{
    bool accepted = false;

    /* Clear pending response */
    s_resp_len = 0;
    s_resp_declared_len = 0;
    s_resp_mode = SCPI_READ_NONE;
    s_resp_buf[0] = '\0';

    if (!is_valid_scpi_buffer(buf)) {
        DBGF("SCPI: rejected non-printable input\n");
        return false;
    }

    DBGF("SCPI WRITE: %s\n", buf);

    /* Strip trailing whitespace / NUL padding */
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n' ||
                       buf[len-1] == ' ')) {
        buf[--len] = '\0';
    }

    /*
     * Split multi-command sequences by ';'.
     * Each segment is handled in channel context: if a segment
     * begins with Cn: the channel is updated for that segment.
     * The channel context persists within the overall command string
     * to match Siglent scope behaviour (e.g. "C1:OUTP LOAD,50;BSWV …")
     */
    uint8_t ch = 1;          /* default channel */
    char   *seg = buf;
    char   *p   = buf;

    /* Fast path: IDN-SGLT-PRI? with no channel prefix */
    char *trimmed = trim_segment(buf);
    if (match_exact_command(trimmed, "IDN-SGLT-PRI?")) {
        return handle_one_cmd(ch, trimmed);
    }

    /* Detect leading C1:/C2: channel and strip before splitting */
    const char *rest;
    if ((rest = match_prefix(trimmed, "C1:")) != NULL) { ch = 1; seg = (char *)rest; p = seg; }
    else if ((rest = match_prefix(trimmed, "C2:")) != NULL) { ch = 2; seg = (char *)rest; p = seg; }
    else { seg = trimmed; p = trimmed; }

    while (*p) {
        if (*p == ';') {
            *p = '\0';
            char *sub = trim_segment(seg);
            if (*sub && handle_one_cmd(ch, sub)) accepted = true;
            seg = p + 1;
        }
        p++;
    }
    /* handle last (or only) segment */
    char *sub = trim_segment(seg);
    if (*sub && handle_one_cmd(ch, sub)) accepted = true;
    return accepted;
}

/* ── Public: retrieve pending DEVICE_READ response ──────────────────────── */
int scpi_read_response(char *dst, int dst_max, uint32_t *declared_len, ScpiReadMode *mode)
{
    if (s_resp_len <= 0) {
        if (dst_max > 0) dst[0] = '\0';
        if (declared_len) *declared_len = 0;
        if (mode) *mode = SCPI_READ_NONE;
        return 0;
    }
    int n = s_resp_len < dst_max - 1 ? s_resp_len : dst_max - 1;
    memcpy(dst, s_resp_buf, (size_t)n);
    dst[n] = '\0';
    if (declared_len) *declared_len = s_resp_declared_len ? s_resp_declared_len : (uint32_t)n;
    if (mode) *mode = s_resp_mode;
    /* Clear after read */
    s_resp_len = 0;
    s_resp_declared_len = 0;
    s_resp_mode = SCPI_READ_NONE;
    s_resp_buf[0] = '\0';
    return n;
}
