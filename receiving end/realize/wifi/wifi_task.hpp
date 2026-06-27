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
const char *wifi_get_current_ip(void);
bool wifi_is_sta_connected(void);
