#include "wifi_types.hpp"
#include "nv_recv.hpp"
#include "securec.h"

namespace sed_ws63 {

softapconfig::softapconfig()
{
    // 从 NV 读取用户配置的 SoftAP 名称和密码
    softap_config_nv_t cfg;
    nv_recv_read_ap(&cfg);
    if (cfg.ap_name[0] != '\0') {
        (void)strncpy_s(ssid, sizeof(ssid), (const char *)cfg.ap_name, sizeof(ssid) - 1);
    }
    if (cfg.ap_password[0] != '\0') {
        (void)strncpy_s(password, sizeof(password), (const char *)cfg.ap_password, sizeof(password) - 1);
    }
}

void softapconfig::set_ssid(const char *new_ssid)
{
    if (new_ssid == nullptr) {
        return;
    }
    nv_recv_update_ap_name(new_ssid);
}

void softapconfig::set_password(const char *new_password)
{
    if (new_password == nullptr) {
        return;
    }
    nv_recv_update_ap_pwd(new_password);
}

stacredential::stacredential()
{
    // SED : 构造函数中可以包含一些默认值或者初始化逻辑，当前先保持空实现
}

void stacredential::set_ssid(const char *new_ssid)
{
    if (new_ssid == nullptr) {
        return;
    }
    nv_recv_update_sta_ssid(new_ssid);
}

void stacredential::set_password(const char *new_password)
{
    if (new_password == nullptr) {
        return;
    }
    nv_recv_update_sta_pwd(new_password);
}

}