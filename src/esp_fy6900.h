#ifndef ESP_FY6900_H
#define ESP_FY6900_H

#include <stdint.h>

/*
 * esp_fy6900.h – Feeltech FY6900 AWG driver (later firmware, Hz format)
 * ======================================================================
 * Key difference from older FY6800 / early FY6900 drivers:
 *   FREQUENCY is sent as Hz with explicit decimal point (6 places):
 *     "WMF000001000.000000\n"  (format: %015.6f  → 8 integer + . + 6 decimal = 16 chars total but zero-padded to 15+dot)
 *   Older format (FY6800 / pre-1.4 FY6900):
 *     "WMF00001000000000\n"   (uHz as 14-digit integer, no decimal)
 *
 * Serial port assignment:
 *   Serial (UART0 / GPIO1-TX / GPIO3-RX) is used exclusively for AWG.
 *   NO debug text is ever written to Serial in release builds.
 *
 * Internal state cache:
 *   DeviceState mirrors the AWG's current parameters so BSWV? queries
 *   can be answered locally without reading back from the AWG.
 *
 * Wave type encoding (FY6900 matches FY6800):
 *   0=Sine 1=Square 2=CMOS 3=AdjPulse 4=DC 5=Tri 6=PosRamp 7=NegRamp
 *   25=Noise  (used as placeholder for unsupported types)
 */

/* ── Wave type enumeration (Siglent names → FY index) ─────────────────── */
typedef enum {
    AWG_SINE      = 0,
    AWG_SQUARE    = 1,
    AWG_TRIANGLE  = 5,
    AWG_POSRAMP   = 6,
    AWG_NEGRAMP   = 7,
    AWG_NOISE     = 25,
    AWG_WAVE_LAST
} AwgWaveType;

typedef enum {
    AWG_LOAD_50 = 0,
    AWG_LOAD_75,
    AWG_LOAD_HIZ
} AwgLoadMode;

/* ── Internal state cache ─────────────────────────────────────────────── */
typedef struct {
    uint8_t    ch1_on;         /* 0 = off, 1 = on */
    uint8_t    ch2_on;
    AwgWaveType ch1_wave;
    AwgWaveType ch2_wave;
    double     ch1_freq_hz;   /* Hz, e.g. 1000.0 */
    double     ch2_freq_hz;
    double     ch1_ampl_v;    /* Volts peak-to-peak, e.g. 2.0 */
    double     ch2_ampl_v;
    double     ch1_ofst_v;    /* Volts DC offset */
    double     ch2_ofst_v;
    double     ch1_drive_ampl_v; /* Compensated volts sent to AWG */
    double     ch2_drive_ampl_v;
    double     ch1_drive_ofst_v;
    double     ch2_drive_ofst_v;
    double     ch1_phase_deg; /* degrees, signed */
    double     ch2_phase_deg;
    AwgLoadMode ch1_load;
    AwgLoadMode ch2_load;
} AwgState;

extern AwgState g_awg;

/* ── Init: configure Serial baud, send safe default state ──────────────── */
void fy_init(void);

/* Runtime ownership of UART0 for AWG traffic. */
bool fy_is_enabled(void);
bool fy_is_supported_baud(uint32_t baud);
void fy_set_enabled(bool enabled);
uint32_t fy_get_baud(void);
void fy_set_baud(uint32_t baud);
int  fy_read_available(char *buf, int buf_max);
void fy_force_resync(void);
void fy_restore_startup_state(void);

/* ── Per-channel setters ────────────────────────────────────────────────── */
void fy_set_output (uint8_t ch, uint8_t on);
void fy_set_wave   (uint8_t ch, AwgWaveType wave);
void fy_set_freq   (uint8_t ch, double freq_hz);
void fy_set_ampl   (uint8_t ch, double ampl_v);
void fy_set_offset (uint8_t ch, double ofst_v);
void fy_set_phase  (uint8_t ch, double phase_deg);
void fy_set_load   (uint8_t ch, AwgLoadMode load);
AwgLoadMode fy_get_load(uint8_t ch);
double fy_get_drive_ampl(uint8_t ch);
double fy_get_drive_offset(uint8_t ch);
const char *fy_load_to_text(AwgLoadMode load);

/* ── Translate Siglent WVTP string → AwgWaveType ────────────────────────── */
AwgWaveType fy_wave_from_siglent(const char *name);
const char *fy_wave_to_siglent (AwgWaveType w);

/* ── Diagnostic: send a raw command string, read and return the reply.
 *   Only used when compiled with DEBUG_BUILD.  Returns bytes read.   ── */
int  fy_raw_cmd(const char *cmd, char *reply_buf, int reply_max);

#endif /* ESP_FY6900_H */
