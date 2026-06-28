#include "dlan.hpp"
#include "iis.hpp"
#include "minimp3.hpp"
#include "provisioner.hpp"
#include "http_control.hpp"

void *wifi_task(void *arg);
void *dlna_task(void *arg);
void *minimp3_task(void *arg);

// 供 HTTP status 接口获取当前 WiFi 信息
const char *wifi_get_current_ssid(void);
const char *wifi_get_current_ap_name(void);
const char *wifi_get_current_ip(void);
bool wifi_is_sta_connected(void);

// 更新 STA 凭据（同时写入 NV），下次连接周期生效
void wifi_update_sta_credentials(const char *ssid, const char *password);

// 更新 SoftAP 配置（同时写入 NV），下次开启热点生效
void wifi_update_ap_config(const char *name, const char *password);
