#include "sta.hpp"

namespace sed_ws63 {

sta *sta::instance = nullptr;

sta::sta()
{
    instance = this; // 将当前实例指针赋值给静态成员变量，供静态回调函数使用

    wifi_tool::copy_str(wifi_ifname, sizeof(wifi_ifname), "wlan0");

    // 初始化信号量
    osal_sem_init(&scan_sem_, 0);
    osal_sem_init(&connect_sem_, 0);

    // 注册wifi事件回调
    wifi_event_stru cb = {0};
    cb.wifi_event_connection_changed = wifi_connection_changed_callback;
    cb.wifi_event_scan_state_changed = wifi_scan_done_callback;

    errcode_t ret = wifi_register_event_cb(&cb);
    if (ret != 0) {
        osal_printk("[Sta] 注册事件回调失败，错误码: %u\n", ret);
    }
}

sta::~sta()
{
    // 断开连接
    sta_disconnect();
    // 销毁信号量
    osal_sem_destroy(&scan_sem_);
    osal_sem_destroy(&connect_sem_);
    if (instance == this) {
        instance = nullptr;
    }
}

errcode_t sta::sta_connect(const stacredential &cred)
{
    // 保存凭证
    memcpy_s(&current_cred_, sizeof(current_cred_), &cred, sizeof(cred));
    // 重置连接状态和自动重连标志
    is_connected_ = false;
    reset_semaphores();

    return do_connect_once(cred);
}

void sta::sta_disconnect()
{
    auto_reconnect_ = false;
    (void)wifi_sta_disable();
    is_connected_ = false;
}

void sta::enable_auto_reconnect()
{
    auto_reconnect_ = true;
}

void sta::disable_auto_reconnect()
{
    auto_reconnect_ = false;
}

bool sta::is_connected() const
{
    return is_connected_;
}

netif *sta::get_netif()
{
    return netifapi_netif_find_by_name(wifi_ifname);
}

void sta::reset_semaphores()
{
    // 销毁旧的信号量并重新初始化
    osal_sem_destroy(&scan_sem_);
    osal_sem_destroy(&connect_sem_);
    osal_sem_init(&scan_sem_, 0);
    osal_sem_init(&connect_sem_, 0);
}

errcode_t sta::do_connect_once(const stacredential &cred)
{
    // 使能sta
    errcode_t ret = wifi_sta_enable();
    if (ret != 0) {
        wifi_sta_disable();
        osal_msleep(100);
        ret = wifi_sta_enable();
        if (ret != 0) {
            return ret; // 重连也失败，返回错误码
        }
    }

    // 配置自动重连
    if (auto_reconnect_) {
        wifi_sta_set_reconnect_policy(1, 10, 10, 100);
    } else {
        wifi_sta_set_reconnect_policy(0, 0, 0, 0);
    }

    // 发起定向扫描
    ret = do_scan(cred.ssid);
    if (ret != 0) {
        return ret;
    }
    // 等待扫描完成
    osal_sem_down(&scan_sem_);

    // 从扫描结果中获取待连接网络信息并连接
    wifi_sta_config_stru sta_config = {0};
    ret = find_and_build_config(cred, &sta_config);
    if (ret != 0) {
        return ret;
    }

    // 启动dhcp
    netif *netif_p = get_netif();
    if (netif_p == nullptr) {
        return 0x01; // 没有找到接口，返回错误码
    }
    ret = netifapi_dhcp_start(netif_p);
    if (ret != 0) {
        return ret;
    }

    is_connected_ = true;
    return ERRCODE_SUCC;
}

errcode_t sta::do_scan(const char *ssid)
{
    wifi_scan_params_stru params = {0};
    uint32_t ssid_len = strlen(ssid);
    if (ssid_len == 0 || ssid_len >= sizeof(params.ssid)) {
        return 0x02; // SSID长度不合法
    }
    if (memcpy_s(params.ssid, sizeof(params.ssid), ssid, ssid_len + 1) != 0) {
        return 0x03; // 复制SSID失败
    }
    params.ssid_len = ssid_len;
    params.scan_type = WIFI_SSID_SCAN;

    errcode_t ret = wifi_sta_scan_advance(&params);
    if (ret != 0) {
        return ret; // 扫描错误，返回错误码
    }
    return ERRCODE_SUCC;
}

errcode_t sta::find_and_build_config(const stacredential &cred, wifi_sta_config_stru *sta_config)
{
    int32_t buffer_size = sizeof(wifi_scan_info_stru) * max_scan_num;
    wifi_scan_info_stru *results = static_cast<wifi_scan_info_stru *>(osal_kmalloc(buffer_size, OSAL_GFP_ATOMIC));
    if (results == nullptr) {
        return 0x04; // 内存分配失败
    }
    memset_s(results, buffer_size, 0, buffer_size);

    uint32_t scan_num = max_scan_num;
    errcode_t ret = wifi_sta_get_scan_info(results, &scan_num);
    if (ret != 0) {
        osal_kfree(results);
        return ret; // 获取扫描结果失败，返回错误码
    }

    bool found = false;
    uint32_t found_index = 0;
    uint32_t ssid_len = strlen(cred.ssid);

    for (int i = 0; i < scan_num; i++) {
        if (strlen(results[i].ssid) == ssid_len && memcmp(results[i].ssid, cred.ssid, ssid_len) == 0) {
            found = true;
            found_index = i;
            break;
        }
    }

    if (!found) {
        osal_kfree(results);
        return 0x05; // 没有找到目标AP，返回错误码
    }

    // 填充连接配置
    uint32_t password_len = strlen(cred.password);
    bool ok = (memcpy_s(sta_config->ssid, sizeof(sta_config->ssid), cred.ssid, ssid_len) == 0) &&
              (memcpy_s(sta_config->bssid, sizeof(sta_config->bssid), results[found_index].bssid, 6) == 0) &&
              (memcpy_s(sta_config->pre_shared_key, sizeof(sta_config->pre_shared_key), cred.password, password_len) == 0);

    if (!ok) {
        osal_kfree(results);
        return 0x06; // 填充错误
    }

    sta_config->security_type = results[found_index].security_type;
    sta_config->ip_type = DHCP; 

    osal_kfree(results);
    return ERRCODE_SUCC;
}

void sta::wifi_connection_changed_callback(int32_t state,
                                                 const wifi_linked_info_stru *scan_result,
                                                 int32_t reason_code)
{
    if (instance == nullptr) {
        return; // 没有实例，无法处理回调
    }
    if (state == 1){
        osal_sem_up(&instance->connect_sem_); // 连接成功，释放连接完成信号量
    }else{
        instance->is_connected_ = false; // 连接断开，更新连接状态
    }
}

void sta::wifi_scan_done_callback(int32_t state, int32_t size)
{
    if (instance == nullptr) {
        return; // 没有实例，无法处理回调
    }
    osal_sem_up(&instance->scan_sem_); // 扫描完成，释放扫描完成信号量
}

} // namespace sed_ws63