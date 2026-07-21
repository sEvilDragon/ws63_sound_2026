#include "wifi.hpp"


// 定义静态成员变量
osal_semaphore wifi::scan_done_sem;
osal_semaphore wifi::connect_done_sem;
bool wifi::connect_done = false;
std::array<char, wifi::wifi_ssid_max_size> wifi::wifi_scan_expect_ssid = {"WS63_TEST_INVALID_AP"};
std::array<char, wifi::wifi_password_max_size> wifi::wifi_scan_expect_password = {"wrong_password_2026"};

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

    // 开启softap配网
    ret = softap_start();
    if (ret != 0) {
        osal_printk("SoftAP配网启动失败，错误码: %u\n", ret);
        return;
    }

    // 开启UDP接收配网信息
    while (!udp()) {
        osal_msleep(100);
    }

    // 结束softap
    wifi_softap_disable();
    osal_msleep(1000); // 等待SoftAP完全关闭并让射频状态稳定

    // 初始化STA模式
    // 循环尝试初始化STA模式，直到成功为止
    sta_start();

    is_ready = true; // WiFi准备就绪
}

void wifi::restart_get_wifi()
{
    wifi_sta_disable();
    osal_msleep(100); // 等待STA完全关闭
    
    // 启动softap
    errcode_t ret = softap_start();
    if (ret != 0) {
        osal_printk("SoftAP配网启动失败，错误码: %u\n", ret);
        return;
    }

    // 开启UDP接收配网信息
    while (!udp()) {
        osal_msleep(100);
    }

    // 结束softap
    wifi_softap_disable();
    osal_msleep(1000); // 等待SoftAP完全关闭并让射频状态稳定

    sta_start();
}

void wifi::sta_start()
{
    while (true) {
        errcode_t ret = sta_init();
        if (ret != 0) {
            // 失败后先关闭STA，避免下一轮enable直接返回失败
            (void)wifi_sta_disable();
            osal_msleep(300);
            sem_restart();
            continue;
        }
        osal_msleep(100); // 等待一段时间，避免频繁重试
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
    std::array<char, wifi_password_max_size> wifi_scan_expect_password_ = wifi_scan_expect_password;
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
    if (memcpy_s(sta_config->pre_shared_key, wifi_password_max_size, wifi_scan_expect_password_.data(), password_len) !=
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
    std::array<char, wifi_ifname_max_size + 1> wifi_ifname = wifi::wifi_ifname_sta;
    // STA配置结构体，在后续的函数中会进行具体赋值和填充
    wifi_sta_config_stru sta_config = {0};
    // 指向网络接口的指针，后续会用于获取IP地址等操作
    netif *netif_p = nullptr;

    errcode_t enable_ret = wifi_sta_enable();
    if (enable_ret != 0) {
        // 某些情况下STA残留在异常状态，先关闭再尝试一次恢复
        osal_printk("网络STA使能错误，错误码: %u，尝试恢复\n", enable_ret);
        (void)wifi_sta_disable();
        osal_msleep(100);
        enable_ret = wifi_sta_enable();
        if (enable_ret != 0) {
            osal_printk("网络STA恢复后仍使能失败，错误码: %u\n", enable_ret);
            return ERRCODE_FAIL;
        }
    }

    // 设置网络自动重新连接
    wifi_sta_set_reconnect_policy(1, 10, 10, 100);

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
        // 说明连接断开了，应该重新连接
        wifi_sta_disable();
        osal_msleep(300);
        sta_start();
    }
}

void wifi::wifi_scan_done_callback(int32_t state, int32_t size)
{
    osal_sem_up(&scan_done_sem);
}

errcode_t wifi::softap_start()
{
    std::array<char, wifi_ssid_max_size> wifi_softap_ssid_ = wifi::wifi_softap_ssid;
    std::array<char, wifi_password_max_size> wifi_softap_password_ = wifi::wifi_softap_password;
    std::array<char, wifi_ifname_max_size + 1> wifi_ifname_softap_ = wifi::wifi_ifname_softap;
    uint32_t ssid_len = strlen(wifi_softap_ssid_.data());
    uint32_t password_len = strlen(wifi_softap_password_.data());
    softap_config_stru hapd_conf = {0};
    softap_config_advance_stru config = {0};
    netif *netif_p = nullptr;
    ip4_addr_t st_gw;
    ip4_addr_t st_ipaddr;
    ip4_addr_t st_netmask;

    // 配置ip地址
    IP4_ADDR(&st_gw, wifi_softap_ip[0], wifi_softap_ip[1], wifi_softap_ip[2], wifi_softap_ip[3]);
    IP4_ADDR(&st_ipaddr, wifi_softap_ip[0], wifi_softap_ip[1], wifi_softap_ip[2], wifi_softap_ip[3]);
    IP4_ADDR(&st_netmask, wifi_softap_netmask[0], wifi_softap_netmask[1], wifi_softap_netmask[2],
             wifi_softap_netmask[3]);

    // 配置SoftAP基本参数
    (void)memcpy_s(hapd_conf.ssid, sizeof(hapd_conf.ssid), wifi_softap_ssid_.data(), ssid_len);
    (void)memcpy_s(hapd_conf.pre_shared_key, wifi_password_max_size, wifi_softap_password_.data(), password_len);
    hapd_conf.security_type = softap_security_type;
    hapd_conf.channel_num = softap_channel_num;
    hapd_conf.wifi_psk_type = 0;

    // 配置SoftAP网络参数
    config.beacon_interval = softap_beacon_interval;
    config.dtim_period = softap_dtim_period;
    config.gi = softap_gi;
    config.protocol_mode = softap_protocol_mode;
    config.group_rekey = softap_group_rekey;
    config.hidden_ssid_flag = softap_hidden_ssid_flag;

    errcode_t ret = wifi_set_softap_config_advance(&config);
    if (ret != 0) {
        osal_printk("SoftAP设置高级配置失败，错误码: %u\n", ret);
        return ret;
    }

    ret = wifi_softap_enable(&hapd_conf);
    if (ret != 0) {
        osal_printk("SoftAP使能失败，错误码: %u\n", ret);
        return ret;
    }

    // 配置DHCP服务器
    netif_p = netif_find(wifi_ifname_softap_.data());
    if (netif_p == nullptr) {
        osal_printk("SoftAP接口未找到，接口名称: %s\n", wifi_ifname_softap_.data());
        return ret;
    }
    ret = netifapi_netif_set_addr(netif_p, &st_ipaddr, &st_netmask, &st_gw);
    if (ret != 0) {
        osal_printk("SoftAP设置接口地址失败，错误码: %u\n", ret);
        return ret;
    }
    ret = netifapi_dhcps_start(netif_p, nullptr, 0);
    if (ret != 0) {
        osal_printk("SoftAP启动DHCP服务器失败，错误码: %u\n", ret);
        return ret;
    }
    return ERRCODE_SUCC;
}

bool wifi::udp()
{
    int32_t sfd = udp_sfd;
    sockaddr_in srv_addr = {0};    // 服务器地址结构体
    sockaddr_in client_addr = {0}; // 记录信息来源
    socklen_t client_addr_len = sizeof(client_addr);
    char buffer[udp_buffer] = {0}; // UDP接收缓冲区

    // 创建UDP套接字
    // 第一个参数是地址族，AF_INET表示IPv4；第二个参数是套接字类型，SOCK_DGRAM表示UDP；第三个参数是协议，0表示默认协议
    sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) {
        osal_printk("UDP创建套接字失败\n");
        return false;
    }

    // 配置服务端地址结构体
    srv_addr.sin_family = AF_INET;                             // 地址族设置为IPv4
    srv_addr.sin_addr.s_addr = inet_addr(udp_stack_ip.data()); // 设置服务地址
    srv_addr.sin_port = lwip_htons(udp_stack_port);            // 服务器端口
    errcode_t ret = lwip_bind(sfd, (sockaddr *)&srv_addr, sizeof(srv_addr));
    if (ret != 0) {
        osal_printk("UDP绑定套接字失败 错误码: %d\n", ret);
        lwip_close(sfd);
        return false;
    }

    // 堵塞并等待接收数据，recvfrom函数会将接收到的数据存储在buffer中，并将发送方的地址信息存储在client_addr中
    ret = lwip_recvfrom(sfd, buffer, sizeof(buffer), 0, (sockaddr *)&client_addr, &client_addr_len);
    if (ret < 0) {
        osal_printk("UDP接收数据失败\n");
        lwip_close(sfd);
        return false;
    }
    if (ret >= static_cast<int32_t>(sizeof(buffer))) {
        ret = static_cast<int32_t>(sizeof(buffer) - 1);
    }
    buffer[ret] = '\0';

    // 获得数据，并配置网络信息
    // 解析json文件
    cJSON *json = cJSON_Parse(buffer);
    if (json == nullptr) {
        osal_printk("UDP解析JSON失败\n");
        lwip_close(sfd);
        return false;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *password_item = cJSON_GetObjectItem(json, "password");

    if (ssid_item == nullptr || password_item == nullptr) {
        osal_printk("UDP解析JSON失败，缺少必要字段\n");
        lwip_close(sfd);
        cJSON_Delete(json);
        return false;
    }

    // 填充网络信息
    std::array<char, wifi_ssid_max_size> wifi_scan_expect_ssid_set = {0};
    std::array<char, wifi_password_max_size> wifi_scan_expect_password_set = {0};

    ret = memcpy_s(wifi_scan_expect_ssid_set.data(), wifi_ssid_max_size, ssid_item->valuestring,
                   strlen(ssid_item->valuestring) + 1);
    if (ret != 0) {
        osal_printk("UDP复制SSID失败\n");
        lwip_close(sfd);
        cJSON_Delete(json);
        return false;
    }

    ret = memcpy_s(wifi_scan_expect_password_set.data(), wifi_password_max_size, password_item->valuestring,
                   strlen(password_item->valuestring) + 1);
    if (ret != 0) {
        osal_printk("UDP复制密码失败\n");
        lwip_close(sfd);
        cJSON_Delete(json);
        return false;
    }

    // 将获得的网络信息赋值给wifi类的静态成员变量，供后续连接使用
    wifi_scan_expect_ssid = wifi_scan_expect_ssid_set;  
    wifi_scan_expect_password = wifi_scan_expect_password_set;

    // 清理资源
    cJSON_Delete(json);
    lwip_close(sfd);

    return true;
}
