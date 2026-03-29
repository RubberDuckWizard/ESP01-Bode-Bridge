#ifndef ESP_PERSIST_H
#define ESP_PERSIST_H

#include <stdint.h>

/*
 * esp_persist.h – EEPROM-backed runtime configuration
 * ====================================================
 * Versioned struct with CRC-16 (CCITT) checksum.
 * Total struct size ≈ 120 bytes, fits in 256-byte EEPROM window.
 *
 * On first boot (invalid CRC or wrong magic/version) defaults from
 * esp_config.h are written.
 */

#define CONFIG_MAGIC    0xB0DE
#define CONFIG_VERSION  2
#define CONFIG_EEPROM_ADDR  0
#define CONFIG_EEPROM_SIZE  256

#pragma pack(push, 1)
struct EspConfig {
    uint16_t magic;
    uint8_t  version;
    uint8_t  use_dhcp;      /* 0 = static, 1 = DHCP */
    char     ssid[33];      /* null-terminated, max 32 chars */
    char     psk[65];       /* null-terminated, max 64 chars */
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    char     device_hostname[25];
    char     friendly_name[25];
    char     idn_response_name[17];
    char     fallback_ap_ssid[25];
    char     fallback_ap_password[17];
    uint8_t  sta_mac_set;
    uint8_t  ap_mac_set;
    uint8_t  sta_mac[6];
    uint8_t  ap_mac[6];
    uint32_t awg_baud;
    uint16_t crc;           /* CRC-16 over all preceding fields */
};
#pragma pack(pop)

extern EspConfig g_config;

/* Initialise – load from EEPROM or write defaults on first boot. */
void     config_init();

/* Load from EEPROM; returns true if CRC and version are valid. */
bool     config_load();

/* Compute CRC over all fields except the crc member itself. */
uint16_t config_crc(const EspConfig *c);

/* Write current g_config to EEPROM (updates CRC first). */
void     config_save();

/* Overwrite g_config with compile-time defaults and save. */
void     config_reset_defaults();

/* Returns true if the EEPROM contents were valid before config_init(). */
bool     config_store_was_valid();

/* Returns true if the current g_config content matches its CRC. */
bool     config_current_is_valid();

#endif /* ESP_PERSIST_H */
