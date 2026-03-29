#include <EEPROM.h>
#include <string.h>
#include "esp_persist.h"
#include "esp_config.h"

#pragma pack(push, 1)
struct EspConfigV1 {
    uint16_t magic;
    uint8_t  version;
    uint8_t  use_dhcp;
    char     ssid[33];
    char     psk[65];
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    uint16_t crc;
};
#pragma pack(pop)

EspConfig g_config;
static bool s_store_was_valid = false;

static uint16_t crc16_ccitt(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ── CRC-16 CCITT (polynomial 0x1021, init 0xFFFF) ─────────────────────── */
uint16_t config_crc(const EspConfig *c)
{
    return crc16_ccitt(c, sizeof(EspConfig) - sizeof(uint16_t));
}

static uint16_t config_crc_v1(const EspConfigV1 *c)
{
    return crc16_ccitt(c, sizeof(EspConfigV1) - sizeof(uint16_t));
}

/* ── Write compile-time defaults into g_config ──────────────────────────── */
void config_reset_defaults()
{
    static const uint8_t def_ip[]   = { DEF_IP   };
    static const uint8_t def_mask[] = { DEF_MASK };
    static const uint8_t def_gw[]   = { DEF_GW  };
    static const uint8_t def_dns[]  = { DEF_DNS };

    memset(&g_config, 0, sizeof(g_config));
    g_config.magic    = CONFIG_MAGIC;
    g_config.version  = CONFIG_VERSION;
    g_config.use_dhcp = DEF_USE_DHCP;

    strncpy(g_config.ssid, DEF_SSID, sizeof(g_config.ssid) - 1);
    strncpy(g_config.psk,  DEF_PSK,  sizeof(g_config.psk)  - 1);
    strncpy(g_config.device_hostname, DEF_DEVICE_HOSTNAME, sizeof(g_config.device_hostname) - 1);
    strncpy(g_config.friendly_name, DEF_FRIENDLY_NAME, sizeof(g_config.friendly_name) - 1);
    strncpy(g_config.idn_response_name, DEF_IDN_RESPONSE_NAME, sizeof(g_config.idn_response_name) - 1);
    strncpy(g_config.fallback_ap_ssid, DEF_FALLBACK_AP_SSID, sizeof(g_config.fallback_ap_ssid) - 1);
    strncpy(g_config.fallback_ap_password, DEF_FALLBACK_AP_PASSWORD, sizeof(g_config.fallback_ap_password) - 1);

    memcpy(g_config.ip,   def_ip,   4);
    memcpy(g_config.mask, def_mask, 4);
    memcpy(g_config.gw,   def_gw,  4);
    memcpy(g_config.dns,  def_dns, 4);

    g_config.sta_mac_set = 0;
    g_config.ap_mac_set  = 0;
    memset(g_config.sta_mac, 0, sizeof(g_config.sta_mac));
    memset(g_config.ap_mac, 0, sizeof(g_config.ap_mac));
    g_config.awg_baud = AWG_BAUD_RATE;

    g_config.crc = config_crc(&g_config);
}

/* ── Load from EEPROM; return true if valid ─────────────────────────────── */
bool config_load()
{
    EspConfigV1 old_config;

    EEPROM.begin(CONFIG_EEPROM_SIZE);
    EEPROM.get(CONFIG_EEPROM_ADDR, g_config);

    if (g_config.magic == CONFIG_MAGIC &&
        g_config.version == CONFIG_VERSION &&
        config_crc(&g_config) == g_config.crc) {
        g_config.ssid[sizeof(g_config.ssid) - 1] = '\0';
        g_config.psk[sizeof(g_config.psk) - 1] = '\0';
        g_config.device_hostname[sizeof(g_config.device_hostname) - 1] = '\0';
        g_config.friendly_name[sizeof(g_config.friendly_name) - 1] = '\0';
        g_config.idn_response_name[sizeof(g_config.idn_response_name) - 1] = '\0';
        g_config.fallback_ap_ssid[sizeof(g_config.fallback_ap_ssid) - 1] = '\0';
        g_config.fallback_ap_password[sizeof(g_config.fallback_ap_password) - 1] = '\0';
        return true;
    }

    EEPROM.get(CONFIG_EEPROM_ADDR, old_config);
    if (old_config.magic != CONFIG_MAGIC || old_config.version != 1) return false;
    if (config_crc_v1(&old_config) != old_config.crc) return false;

    old_config.ssid[sizeof(old_config.ssid) - 1] = '\0';
    old_config.psk[sizeof(old_config.psk) - 1] = '\0';

    config_reset_defaults();
    g_config.use_dhcp = old_config.use_dhcp;
    strncpy(g_config.ssid, old_config.ssid, sizeof(g_config.ssid) - 1);
    strncpy(g_config.psk, old_config.psk, sizeof(g_config.psk) - 1);
    memcpy(g_config.ip, old_config.ip, sizeof(g_config.ip));
    memcpy(g_config.mask, old_config.mask, sizeof(g_config.mask));
    memcpy(g_config.gw, old_config.gw, sizeof(g_config.gw));
    memcpy(g_config.dns, old_config.dns, sizeof(g_config.dns));
    DBG("Config migrated from v1 (not yet saved)");
    return true;
}

/* ── Write g_config to EEPROM ────────────────────────────────────────────── */
void config_save()
{
    g_config.magic   = CONFIG_MAGIC;
    g_config.version = CONFIG_VERSION;
    g_config.crc     = config_crc(&g_config);

    EEPROM.begin(CONFIG_EEPROM_SIZE);
    EEPROM.put(CONFIG_EEPROM_ADDR, g_config);
    EEPROM.commit();
}

bool config_store_was_valid()
{
    return s_store_was_valid;
}

bool config_current_is_valid()
{
    return config_crc(&g_config) == g_config.crc;
}

/* ── Initialise: load EEPROM or install defaults ────────────────────────── */
void config_init()
{
    s_store_was_valid = config_load();

    if (!s_store_was_valid) {
        DBG("EEPROM empty/invalid – using defaults in RAM");
        config_reset_defaults();
    } else {
        DBG("Config loaded from EEPROM");
    }
}
