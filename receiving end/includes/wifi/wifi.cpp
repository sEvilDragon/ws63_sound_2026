#include "wifi.hpp"

// 定义静态成员变量
osal_semaphore wifi::scan_done_sem;
osal_semaphore wifi::connect_done_sem;
bool wifi::connect_done = false;
std::array<char, wifi::wifi_ssid_max_size> wifi::wifi_scan_expect_ssid = {"OPPO Find X8 972E"};
std::array<char, wifi::wifi_ssid_max_size> wifi::wifi_scan_expect_password = {"mytc4386"};

wifi::wifi()
{
    // 初始化信号量
    sem_init();

    // 注册事件回调
    wifi_event_stru wifi_event_cb = {0};
    wifi_event_cb.wifi_event_connection_changed = wifi_connection_changed_callback;
    wifi_event_cb.wifi_event_scan_state_changed = wifi_scan_done_callback;
    errcode_t ret = wifi_register_event_cb(&wifi_event_cb);
    if (ret != 0) {
        osal_printk("网络STA注册事件回调失败，错误码: %u\n", ret);
        return;
    }

    // 初始化网络
    while (wifi_is_wifi_inited() == 0) {
        osal_msleep(100);
    }

    // 初始化STA模式
    // 循环尝试初始化STA模式，直到成功为止
    sta_start();
}

void wifi::sta_start()
{
    while (true) {
        errcode_t ret = sta_init();
        if (ret != 0) {
            sem_restart();
            continue;
        }
        break;
    }
}

void wifi::sem_init()
{
    osal_sem_init(&scan_done_sem, 0);
    osal_sem_init(&connect_done_sem, 0);
    connect_done = false;
}

void wifi::sem_restart()
{
    osal_sem_destroy(&scan_done_sem);
    osal_sem_destroy(&connect_done_sem);
    sem_init();
}

errcode_t wifi::wifi_start_scan()
{
    // 开启网络定向扫描——扫描指定网络是否存在
    wifi_scan_params_stru scan_params = {0};
    uint32_t ssid_len = strlen(wifi_scan_expect_ssid.data());
    if (ssid_len == 0 || ssid_len >= sizeof(scan_params.ssid)) {
        osal_printk("网络STA扫描参数非法，SSID长度: %u\n", ssid_len);
        return ERRCODE_FAIL;
    }

    errcode_t ret = memcpy_s(scan_params.ssid, sizeof(scan_params.ssid), wifi_scan_expect_ssid.data(), ssid_len + 1);
    if (ret != 0) {
        osal_printk("网络STA复制SSID失败，错误码: %u\n", ret);
        return ret;
    }
    scan_params.ssid_len = ssid_len;
    scan_params.scan_type = wifi_scan_type;
    ret = wifi_sta_scan_advance(&scan_params);
    if (ret != 0) {
        osal_printk("网络STA扫描错误，错误码: %u\n", ret);
        return ret;
    }
    return 0;
}

errcode_t wifi::wifi_get_network_to_connect(wifi_sta_config_stru *sta_config)
{
    std::array<char, wifi_ssid_max_size> wifi_scan_expect_ssid_ = wifi_scan_expect_ssid;
    std::array<char, wifi_ssid_max_size> wifi_scan_expect_password_ = wifi_scan_expect_password;
    uint16_t ssid_len = strlen(wifi_scan_expect_ssid_.data());
    uint16_t password_len = strlen(wifi_scan_expect_password_.data());

    uint32_t scan_num_max = wifi::scan_num_max;
    bool is_found = false;
    uint32_t match_index = 0;
    // 获得扫描结果
    int32_t scan_len = sizeof(wifi_scan_info_stru) * scan_num_max;
    wifi_scan_info_stru *scan_result = static_cast<wifi_scan_info_stru *>(osal_kmalloc(scan_len, OSAL_GFP_ATOMIC));
    if (scan_result == nullptr) {
        osal_printk("网络STA分配内存失败\n");
        return ERRCODE_FAIL;
    }
    // 清空缓冲区，以防止后续使用时出现垃圾数据
    memset_s(scan_result, scan_len, 0, scan_len);
    // 获取扫描结果
    errcode_t ret = wifi_sta_get_scan_info(scan_result, &scan_num_max);
    if (ret != 0) {
        // 获取扫描结果失败，释放内存
        osal_kfree(scan_result);
        osal_printk("网络STA获取扫描结果失败，错误码: %u\n", ret);
        return ret;
    }
    for (int i = 0; i < scan_num_max; i++) {
        if (strlen(wifi_scan_expect_ssid_.data()) == strlen(scan_result[i].ssid)) {
            if (memcmp(wifi_scan_expect_ssid_.data(), scan_result[i].ssid, strlen(wifi_scan_expect_ssid_.data())) ==
                0) {
                is_found = true;
                match_index = i;
                break;
            }
        }
    }
    if (!is_found) {
        osal_kfree(scan_result);
        osal_printk("网络STA未找到目标AP\n");
        return ERRCODE_FAIL;
    }

    // 找到目标AP，填充待连接网络信息
    // 填充网络SSID
    if (memcpy_s(sta_config->ssid, wifi_ssid_max_size, wifi_scan_expect_ssid_.data(), ssid_len) != 0) {
        osal_kfree(scan_result);
        osal_printk("网络STA复制SSID失败，错误码: %u\n", ret);
        return ERRCODE_FAIL;
    }
    // 填充网络BSSID
    if (memcpy_s(sta_config->bssid, wifi_mac_size, scan_result[match_index].bssid, wifi_mac_size) != 0) {
        osal_kfree(scan_result);
        osal_printk("网络STA复制BSSID失败，错误码: %u\n", ret);
        return ERRCODE_FAIL;
    }
    // 填充网络安全类型
    sta_config->security_type = scan_result[match_index].security_type;
    // 填充网络预共享密钥
    if (memcpy_s(sta_config->pre_shared_key, wifi_ssid_max_size, wifi_scan_expect_password_.data(), password_len) !=
        0) {
        osal_kfree(scan_result);
        osal_printk("网络STA复制预共享密钥失败，错误码: %u\n", ret);
        return ERRCODE_FAIL;
    }
    // 填充IP分配类型
    sta_config->ip_type = ip_type;

    osal_kfree(scan_result);
    return 0;
}

errcode_t wifi::sta_init()
{
    // 接口名称
    std::array<char, wifi_ifname_max_size + 1> wifi_ifname = {"wlan0"};
    // STA配置结构体，在后续的函数中会进行具体赋值和填充
    wifi_sta_config_stru sta_config = {0};
    // 指向网络接口的指针，后续会用于获取IP地址等操作
    struct netif *netif_p = nullptr;

    errcode_t enable_ret = wifi_sta_enable();
    if (enable_ret != 0) {
        if (wifi_is_sta_enabled() == 1) {
            osal_printk("网络STA已使能，跳过重复使能\n");
        } else {
            osal_printk("网络STA使能错误，错误码: %u\n", enable_ret);
            return ERRCODE_FAIL;
        }
    }

    // 开启扫描
    errcode_t ret = wifi_start_scan();
    if (ret != 0) {
        return ret;
    }

    // 等待扫描完成的信号量
    osal_sem_down(&scan_done_sem);

    // 尝试获得待连接的网络信息
    ret = wifi_get_network_to_connect(&sta_config);
    if (ret != 0) {
        return ret;
    }

    // 获得待连接网络信息后，启动连接
    ret = wifi_sta_connect(&sta_config);
    if (ret != 0) {
        osal_printk("网络STA连接错误，错误码: %u\n", ret);
        return ret;
    }

    // 等待连接完成的信号量
    osal_sem_down(&connect_done_sem);

    // 获得netif指针，后续可以通过netif_p进行IP地址等相关操作
    netif_p = netifapi_netif_find_by_name(wifi_ifname.data());
    if (netif_p == nullptr) {
        osal_printk("网络STA未找到接口，接口名称: %s\n", wifi_ifname.data());
        return ERRCODE_FAIL;
    }
    ret = netifapi_dhcp_start(netif_p);
    if (ret != 0) {
        osal_printk("网络STA设置接口状态错误，错误码: %u\n", ret);
        return ret;
    }
    connect_done = true;

    // SED：扫描、连接、获取IP等功能，暂时未完成
    return 0;
}

errcode_t wifi::wifi_check_dhcp_status(struct netif *netif_p, uint32_t *wait_count, uint32_t max_wait_count)
{
    if (netif_p == nullptr) {
        osal_printk("网络STA检查DHCP状态失败，netif_p为nullptr\n");
        return ERRCODE_FAIL;
    }
    if ((ip_addr_isany(&(netif_p->ip_addr)) == 0) && (*wait_count <= max_wait_count)) {
        return ERRCODE_SUCC;
    }
    if (*wait_count > max_wait_count) {
        osal_printk("网络STA检查DHCP状态超时，等待次数: %u\n", *wait_count);
        return ERRCODE_FAIL;
    }
    return ERRCODE_FAIL;
}

void wifi::wifi_connection_changed_callback(int32_t state,
                                            const wifi_linked_info_stru *scan_result,
                                            int32_t reason_code)
{
    if (state == 1) {
        osal_printk("网络STA连接成功，SSID: %s\n", scan_result->ssid);
        osal_sem_up(&connect_done_sem);
    } else {
        osal_printk("网络STA连接失败，SSID: %s, 原因码: %d\n", scan_result->ssid, reason_code);
    }
}

void wifi::wifi_scan_done_callback(int32_t state, int32_t size)
{
    osal_printk("网络STA扫描完成，扫描结果数量: %d\n", size);
    osal_sem_up(&scan_done_sem);
}