#include "nv_recv.hpp"

extern "C" {
#include "nv.h"
#include "soc_osal.h"
#include "securec.h"
}

#define NV_PRINT_PREFIX "[NV_Recv] "
#define SLE_AUDIO_NV_VERSION 1

static bool s_sle_adpcm_enabled = false;
static bool s_sle_mono_enabled = false;
static bool s_sle_audio_nv_valid = false;

/* ---------- 公开 API ---------- */

void nv_recv_load_all(void)
{
    uint16_t len = 0;
    wifi_sta_config_nv_t sta_cfg;
    errcode_t ret;

    /* --- 加载 STA 凭据 --- */
    (void)memset_s(&sta_cfg, sizeof(sta_cfg), 0, sizeof(sta_cfg));
    ret = uapi_nv_read(NV_KEY_WIFI_STA, sizeof(sta_cfg), &len, (uint8_t *)&sta_cfg);
    if (ret == ERRCODE_SUCC && len == sizeof(sta_cfg)) {
        osal_printk(NV_PRINT_PREFIX "STA cfg: ssid=%s has_pwd=%u\r\n", (const char *)sta_cfg.ssid, (unsigned)(sta_cfg.password[0] != '\0'));
    } else {
        osal_printk(NV_PRINT_PREFIX "STA cfg: no data (ret=%d len=%u), using defaults\r\n", (int)ret, (unsigned)len);
    }

    /* --- 加载 SoftAP 配置 --- */
    softap_config_nv_t ap_cfg;
    (void)memset_s(&ap_cfg, sizeof(ap_cfg), 0, sizeof(ap_cfg));
    ret = uapi_nv_read(NV_KEY_SOFTAP, sizeof(ap_cfg), &len, (uint8_t *)&ap_cfg);
    if (ret == ERRCODE_SUCC && len == sizeof(ap_cfg)) {
        osal_printk(NV_PRINT_PREFIX "SoftAP cfg: name=%s has_pwd=%u\r\n", (const char *)ap_cfg.ap_name, (unsigned)(ap_cfg.ap_password[0] != '\0'));
    } else {
        osal_printk(NV_PRINT_PREFIX "SoftAP cfg: no data (ret=%d len=%u), using defaults\r\n", (int)ret, (unsigned)len);
    }

    /* --- 加载 DLNA 显示名称 --- */
    dlna_config_nv_t dlna_cfg;
    nv_recv_read_dlna(&dlna_cfg);
    if (dlna_cfg.friendly_name[0] != '\0') {
        osal_printk(NV_PRINT_PREFIX "DLNA name: %s\r\n", (const char *)dlna_cfg.friendly_name);
    } else {
        osal_printk(NV_PRINT_PREFIX "DLNA name: no data, using default\r\n");
    }

    /* --- 加载 SLE 音频压缩配置；缺失或非法时保持无损 PCM 默认值 --- */
    sle_audio_config_nv_t sle_cfg;
    (void)memset_s(&sle_cfg, sizeof(sle_cfg), 0, sizeof(sle_cfg));
    len = 0;
    ret = uapi_nv_read(NV_KEY_SLE_AUDIO, sizeof(sle_cfg), &len, (uint8_t *)&sle_cfg);
    if (ret == ERRCODE_SUCC && len == sizeof(sle_cfg) && sle_cfg.version == SLE_AUDIO_NV_VERSION &&
        sle_cfg.adpcm_enabled <= 1 && sle_cfg.mono_enabled <= 1) {
        s_sle_adpcm_enabled = sle_cfg.adpcm_enabled != 0;
        s_sle_mono_enabled = sle_cfg.mono_enabled != 0;
        s_sle_audio_nv_valid = true;
        osal_printk(NV_PRINT_PREFIX "SLE ADPCM: %s, mono: %s\r\n", s_sle_adpcm_enabled ? "ON" : "OFF",
                    s_sle_mono_enabled ? "ON" : "OFF");
    } else {
        s_sle_adpcm_enabled = false;
        s_sle_mono_enabled = false;
        s_sle_audio_nv_valid = false;
        osal_printk(NV_PRINT_PREFIX "SLE audio: no valid data (ret=%d len=%u), default PCM stereo\r\n",
                    (int)ret, (unsigned)len);
    }
}

/* ---------- 单字段更新（读-改-写） ---------- */

void nv_recv_update_sta_ssid(const char *ssid)
{
    if (ssid == NULL)
        return;
    wifi_sta_config_nv_t cfg;
    uint16_t len = 0;
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    /* 读取现有配置，失败则使用全零 */
    (void)uapi_nv_read(NV_KEY_WIFI_STA, sizeof(cfg), &len, (uint8_t *)&cfg);
    (void)strncpy_s((char *)cfg.ssid, WIFI_NV_SSID_MAX_LEN, ssid, WIFI_NV_SSID_MAX_LEN - 1);
    errcode_t ret = uapi_nv_write(NV_KEY_WIFI_STA, (const uint8_t *)&cfg, sizeof(cfg));
    osal_printk(NV_PRINT_PREFIX "STA ssid updated: %s (ret=%d)\r\n", ssid, (int)ret);
}

void nv_recv_update_sta_pwd(const char *password)
{
    if (password == NULL)
        return;
    wifi_sta_config_nv_t cfg;
    uint16_t len = 0;
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    (void)uapi_nv_read(NV_KEY_WIFI_STA, sizeof(cfg), &len, (uint8_t *)&cfg);
    (void)strncpy_s((char *)cfg.password, WIFI_NV_PWD_MAX_LEN, password, WIFI_NV_PWD_MAX_LEN - 1);
    errcode_t ret = uapi_nv_write(NV_KEY_WIFI_STA, (const uint8_t *)&cfg, sizeof(cfg));
    osal_printk(NV_PRINT_PREFIX "STA pwd updated (ret=%d)\r\n", (int)ret);
}

void nv_recv_update_ap_name(const char *name)
{
    if (name == NULL)
        return;
    softap_config_nv_t cfg;
    uint16_t len = 0;
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    (void)uapi_nv_read(NV_KEY_SOFTAP, sizeof(cfg), &len, (uint8_t *)&cfg);
    (void)strncpy_s((char *)cfg.ap_name, WIFI_NV_SSID_MAX_LEN, name, WIFI_NV_SSID_MAX_LEN - 1);
    errcode_t ret = uapi_nv_write(NV_KEY_SOFTAP, (const uint8_t *)&cfg, sizeof(cfg));
    osal_printk(NV_PRINT_PREFIX "SoftAP name updated: %s (ret=%d)\r\n", name, (int)ret);
}

void nv_recv_update_ap_pwd(const char *password)
{
    if (password == NULL)
        return;
    softap_config_nv_t cfg;
    uint16_t len = 0;
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    (void)uapi_nv_read(NV_KEY_SOFTAP, sizeof(cfg), &len, (uint8_t *)&cfg);
    (void)strncpy_s((char *)cfg.ap_password, WIFI_NV_PWD_MAX_LEN, password, WIFI_NV_PWD_MAX_LEN - 1);
    errcode_t ret = uapi_nv_write(NV_KEY_SOFTAP, (const uint8_t *)&cfg, sizeof(cfg));
    osal_printk(NV_PRINT_PREFIX "SoftAP pwd updated (ret=%d)\r\n", (int)ret);
}

/* ---------- 读取 ---------- */

void nv_recv_read_sta(wifi_sta_config_nv_t *out)
{
    if (out == NULL)
        return;
    uint16_t len = 0;
    (void)memset_s(out, sizeof(*out), 0, sizeof(*out));
    errcode_t ret = uapi_nv_read(NV_KEY_WIFI_STA, sizeof(*out), &len, (uint8_t *)out);
    if (ret != ERRCODE_SUCC || len != sizeof(*out)) {
        (void)memset_s(out, sizeof(*out), 0, sizeof(*out));
    }
}

void nv_recv_read_ap(softap_config_nv_t *out)
{
    if (out == NULL)
        return;
    uint16_t len = 0;
    (void)memset_s(out, sizeof(*out), 0, sizeof(*out));
    errcode_t ret = uapi_nv_read(NV_KEY_SOFTAP, sizeof(*out), &len, (uint8_t *)out);
    if (ret != ERRCODE_SUCC || len != sizeof(*out)) {
        (void)memset_s(out, sizeof(*out), 0, sizeof(*out));
    }
}

void nv_recv_read_dlna(dlna_config_nv_t *out)
{
    if (out == NULL)
        return;
    uint16_t len = 0;
    (void)memset_s(out, sizeof(*out), 0, sizeof(*out));
    errcode_t ret = uapi_nv_read(NV_KEY_DLNA, sizeof(*out), &len, (uint8_t *)out);
    if (ret != ERRCODE_SUCC || len != sizeof(*out)) {
        (void)memset_s(out, sizeof(*out), 0, sizeof(*out));
    } else {
        out->friendly_name[DLNA_NV_NAME_MAX_LEN - 1] = '\0';
    }
}

/* ---------- 整体写入 ---------- */

void nv_recv_write_sta(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL)
        return;
    wifi_sta_config_nv_t cfg;
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    (void)strncpy_s((char *)cfg.ssid, WIFI_NV_SSID_MAX_LEN, ssid, WIFI_NV_SSID_MAX_LEN - 1);
    (void)strncpy_s((char *)cfg.password, WIFI_NV_PWD_MAX_LEN, password, WIFI_NV_PWD_MAX_LEN - 1);
    errcode_t ret = uapi_nv_write(NV_KEY_WIFI_STA, (const uint8_t *)&cfg, sizeof(cfg));
    osal_printk(NV_PRINT_PREFIX "STA cfg written: ssid=%s (ret=%d)\r\n", ssid, (int)ret);
}

void nv_recv_write_ap(const char *name, const char *password)
{
    if (name == NULL || password == NULL)
        return;
    softap_config_nv_t cfg;
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    (void)strncpy_s((char *)cfg.ap_name, WIFI_NV_SSID_MAX_LEN, name, WIFI_NV_SSID_MAX_LEN - 1);
    (void)strncpy_s((char *)cfg.ap_password, WIFI_NV_PWD_MAX_LEN, password, WIFI_NV_PWD_MAX_LEN - 1);
    errcode_t ret = uapi_nv_write(NV_KEY_SOFTAP, (const uint8_t *)&cfg, sizeof(cfg));
    osal_printk(NV_PRINT_PREFIX "SoftAP cfg written: name=%s (ret=%d)\r\n", name, (int)ret);
}

int nv_recv_write_dlna_name(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    dlna_config_nv_t cfg;
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    (void)strncpy_s((char *)cfg.friendly_name, DLNA_NV_NAME_MAX_LEN, name, DLNA_NV_NAME_MAX_LEN - 1);
    errcode_t ret = uapi_nv_write(NV_KEY_DLNA, (const uint8_t *)&cfg, sizeof(cfg));
    osal_printk(NV_PRINT_PREFIX "DLNA name written: %s (ret=%d)\r\n", name, (int)ret);
    return (ret == ERRCODE_SUCC) ? 1 : 0;
}

int nv_recv_sle_adpcm_enabled(void)
{
    return s_sle_adpcm_enabled ? 1 : 0;
}

int nv_recv_write_sle_adpcm(uint8_t enabled)
{
    const bool new_value = enabled != 0;
    if (s_sle_audio_nv_valid && new_value == s_sle_adpcm_enabled) {
        return 1;
    }

    sle_audio_config_nv_t cfg = {};
    cfg.version = SLE_AUDIO_NV_VERSION;
    cfg.adpcm_enabled = new_value ? 1 : 0;
    cfg.mono_enabled = s_sle_mono_enabled ? 1 : 0;
    errcode_t ret = uapi_nv_write(NV_KEY_SLE_AUDIO, (const uint8_t *)&cfg, sizeof(cfg));
    if (ret == ERRCODE_SUCC) {
        s_sle_adpcm_enabled = new_value;
        s_sle_audio_nv_valid = true;
    }
    osal_printk(NV_PRINT_PREFIX "SLE ADPCM written: %s (ret=%d)\r\n", new_value ? "ON" : "OFF", (int)ret);
    return (ret == ERRCODE_SUCC) ? 1 : 0;
}

int nv_recv_sle_mono_enabled(void)
{
    return s_sle_mono_enabled ? 1 : 0;
}

int nv_recv_write_sle_mono(uint8_t enabled)
{
    const bool new_value = enabled != 0;
    if (s_sle_audio_nv_valid && new_value == s_sle_mono_enabled) {
        return 1;
    }

    sle_audio_config_nv_t cfg = {};
    cfg.version = SLE_AUDIO_NV_VERSION;
    cfg.adpcm_enabled = s_sle_adpcm_enabled ? 1 : 0;
    cfg.mono_enabled = new_value ? 1 : 0;
    errcode_t ret = uapi_nv_write(NV_KEY_SLE_AUDIO, (const uint8_t *)&cfg, sizeof(cfg));
    if (ret == ERRCODE_SUCC) {
        s_sle_mono_enabled = new_value;
        s_sle_audio_nv_valid = true;
    }
    osal_printk(NV_PRINT_PREFIX "SLE mono written: %s (ret=%d)\r\n", new_value ? "ON" : "OFF", (int)ret);
    return (ret == ERRCODE_SUCC) ? 1 : 0;
}
