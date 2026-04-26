#include "softap.hpp"

namespace sed_ws63 {

errcode_t softap::init()
{
    if (is_started) {
        return 0x01; // 已经启动
    }

    // 配置SoftAP参数
    softap_config_stru config_tp = {0};
    uint32_t ssid_len = strlen(this->config.ssid);
    uint32_t password_len = strlen(this->config.password);
    if (ssid_len == 0 || ssid_len > 32 || password_len > 64) {
        return 0x02; // 配置错误
    }
    if (memcpy_s(config_tp.ssid, sizeof(config_tp.ssid), this->config.ssid, ssid_len) != 0) {
        return 0x03; // ssid复制错误
    }
    if (memcpy_s(config_tp.pre_shared_key, sizeof(config_tp.pre_shared_key), this->config.password, password_len) !=
        0) {
        return 0x04; // password复制错误
    }
    config_tp.security_type = this->config.security_type;
    config_tp.channel_num = this->config.channel_num;
    config_tp.wifi_psk_type = this->config.psk_type;

    // 设置SoftAP高级配置
    softap_config_advance_stru config_advance = {0};
    config_advance.beacon_interval = this->config.beacon_interval;
    config_advance.dtim_period = this->config.dtim_period;
    config_advance.gi = this->config.gi;
    config_advance.protocol_mode = this->config.protocol_mode;
    config_advance.group_rekey = this->config.group_rekey;
    config_advance.hidden_ssid_flag = this->config.hidden_ssid_flag;
    errcode_t ret = wifi_set_softap_config_advance(&config_advance);
    if (ret != 0) {
        return ret; // 设置高级配置失败
    }

    // 启动SoftAP
    ret = wifi_softap_enable(&config_tp);
    if (ret != 0) {
        return ret; // 启动失败
    }

    // 配置接口
    netif *netif_p = netif_find(softapconfig::ifname);
    if (netif_p == nullptr) {
        wifi_softap_disable(); // 启动失败，关闭SoftAP
        return 0x05;           // 接口未找到
    }

    ip4_addr_t ip, netmask, gw;
    IP4_ADDR(&ip, config.ip[0], config.ip[1], config.ip[2], config.ip[3]);
    IP4_ADDR(&netmask, config.netmask[0], config.netmask[1], config.netmask[2], config.netmask[3]);
    IP4_ADDR(&gw, config.gw[0], config.gw[1], config.gw[2], config.gw[3]);

    ret = netifapi_netif_set_addr(netif_p, &ip, &netmask, &gw);
    if (ret != 0) {
        wifi_softap_disable(); // 配置接口失败，关闭SoftAP
        return ret;           // 设置接口地址失败
    }

    // 启动DHCP服务器
    ret = netifapi_dhcps_start(netif_p, nullptr, 0);
    if (ret != 0) {
        wifi_softap_disable(); // 启动DHCP服务器失败，关闭SoftAP
        return ret;           // 启动DHCP服务器失败
    }

    is_started = true; // 记录SoftAP已启动

    return ERRCODE_SUCC; // 成功
}

void softap::deinit()
{
    if (!is_started) {
        return; // 没有启动，无需关闭
    }

    wifi_softap_disable(); // 关闭SoftAP
    is_started = false;   // 记录SoftAP已关闭
}

bool softap::is_enabled() const
{
    return is_started;
}

} // namespace sed_ws63