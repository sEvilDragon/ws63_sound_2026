#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 接收端 NV Key ID */
#define NV_KEY_WIFI_STA 0x5002
#define NV_KEY_SOFTAP 0x5003
#define NV_KEY_DLNA 0x5004

/* NV 存储结构体（与 nv_common_cfg.h 保持一致） */
#define WIFI_NV_SSID_MAX_LEN 33
#define WIFI_NV_PWD_MAX_LEN 65
#define DLNA_NV_NAME_MAX_LEN 64

typedef struct {
    uint8_t ssid[WIFI_NV_SSID_MAX_LEN];
    uint8_t password[WIFI_NV_PWD_MAX_LEN];
} wifi_sta_config_nv_t;

typedef struct {
    uint8_t ap_name[WIFI_NV_SSID_MAX_LEN];
    uint8_t ap_password[WIFI_NV_PWD_MAX_LEN];
} softap_config_nv_t;

typedef struct {
    uint8_t friendly_name[DLNA_NV_NAME_MAX_LEN];
} dlna_config_nv_t;

/**
 * @brief 上电时从 NV 加载所有接收端配置并打印。
 *        应在 osal_kthread_lock() 之前调用。
 */
void nv_recv_load_all(void);

/**
 * @brief 更新 STA SSID（读-改-写）。
 */
void nv_recv_update_sta_ssid(const char *ssid);

/**
 * @brief 更新 STA 密码（读-改-写）。
 */
void nv_recv_update_sta_pwd(const char *password);

/**
 * @brief 更新 SoftAP 名称（读-改-写）。
 */
void nv_recv_update_ap_name(const char *name);

/**
 * @brief 更新 SoftAP 密码（读-改-写）。
 */
void nv_recv_update_ap_pwd(const char *password);

/**
 * @brief 从 NV 读取 STA 凭据（失败返回全零）。
 */
void nv_recv_read_sta(wifi_sta_config_nv_t *out);

/**
 * @brief 从 NV 读取 SoftAP 配置（失败返回全零）。
 */
void nv_recv_read_ap(softap_config_nv_t *out);

/**
 * @brief 从 NV 读取 DLNA friendlyName（失败返回全零）。
 */
void nv_recv_read_dlna(dlna_config_nv_t *out);

/**
 * @brief 整体写入 STA 凭据（同时写入 ssid 和 password）。
 */
void nv_recv_write_sta(const char *ssid, const char *password);

/**
 * @brief 整体写入 SoftAP 配置（同时写入 name 和 password）。
 */
void nv_recv_write_ap(const char *name, const char *password);

/**
 * @brief 持久化 DLNA friendlyName。
 * @return 1 写入成功，0 写入失败。
 */
int nv_recv_write_dlna_name(const char *name);

#ifdef __cplusplus
}
#endif
