#include "dlan.hpp"

// 初始化静态成员变量
std::array<char, 16> dlan::local_ip = {0};
bool dlan::is_ready = false;
uint32_t dlan::ssdp_sock = 0;
uint32_t dlan::http_sock = 0;

void dlan::ssdp_and_http_scan()
{
    while(!is_ready)
    {
        // 等待网络准备就绪，获取本地IP地址等信息
        osal_msleep(100);
    }
    ssdp_ip_get();
    // 建立SSDP（UDP）套接字
    ssdp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ssdp_sock < 0) {
        osal_printk("ssdp进程启动失败\n");
        return;
    }
    ssdp_set();

    // 建立HTTP（TCP）套接字
    http_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (http_sock < 0) {
        osal_printk("http进程启动失败\n");
        return;
    }
    http_set();

    fd_set read_fds;
    int max_fd = (ssdp_sock > http_sock) ? ssdp_sock : http_sock;

    while (true) {
        memset(&read_fds, 0, sizeof(read_fds));
        FD_SET(ssdp_sock, &read_fds); // 把 UDP 监控加进去
        FD_SET(http_sock, &read_fds); // 把 TCP 监控加进去

        int ret = lwip_select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr);

        if (ret < 0) {
            osal_printk("select出错\n");
            continue;
        }

        if (FD_ISSET(ssdp_sock, &read_fds)) {
            ssdp_process();
        }
        if (FD_ISSET(http_sock, &read_fds)) {
            http_process();
        }
    }
}

void dlan::ssdp_set()
{
    uint32_t sock = ssdp_sock;
    // 配置SSDP套接字
    sockaddr_in ssdp_addr = {0};
    ssdp_addr.sin_family = AF_INET;
    ssdp_addr.sin_addr.s_addr = INADDR_ANY;
    ssdp_addr.sin_port = htons(ssdp_port);

    errcode_t ret = bind(sock, (sockaddr *)&ssdp_addr, sizeof(ssdp_addr));
    if (ret != 0) {
        osal_printk("ssdp套接字绑定失败\n");
        lwip_close(sock);
        return;
    }

    // 加入多播组
    ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_ip.data());
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        osal_printk("ssdp套接字加入多播组失败\n");
        lwip_close(sock);
        return;
    }
}

void dlan::http_set()
{
    uint32_t sock = http_sock;

    // 配置HTTP套接字
    sockaddr_in http_addr = {0};
    http_addr.sin_family = AF_INET;
    http_addr.sin_addr.s_addr = INADDR_ANY;
    http_addr.sin_port = htons(http_port);
    errcode_t ret = bind(sock, (sockaddr *)&http_addr, sizeof(http_addr));
    if (ret != 0) {
        osal_printk("http套接字绑定失败\n");
        lwip_close(sock);
        return;
    }

    // 调用listen函数监听HTTP套接字，设置最大连接数为2
    ret = listen(sock, 2);
    if (ret != 0) {
        osal_printk("http套接字监听失败\n");
        lwip_close(sock);
        return;
    }
}

void dlan::ssdp_process()
{
    uint32_t sock = ssdp_sock;
    static std::array<char, 1024> buffer; // SSDP接收缓冲区
    sockaddr_in client_addr = {0};        // 记录SSDP消息来源
    socklen_t client_addr_len = sizeof(client_addr);

    int ret = recvfrom(sock, buffer.data(), buffer.size() - 1, 0, (sockaddr *)&client_addr, &client_addr_len);

    if (ret < 0) {
        osal_printk("ssdp数据接收失败\n");
    }
    return;

    buffer[ret] = '\0';
    // 填充SSDP消息处理逻辑，例如解析SSDP消息并响应
    if (strstr(buffer.data(), "M-SEARCH") && strstr(buffer.data(), "urn:schemas-upnp-org:device:MediaRenderer:1")) {
        std::array<char, 768> response;
        snprintf(response.data(), response.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "CACHE-CONTROL: max-age=%d\r\n"
                 "EXT:\r\n"
                 "LOCATION: http://%s:%u/description.xml\r\n"
                 "SERVER: HiSilicon-WS63_sound DLNA DMR/1.0 UPnP/1.1\r\n"
                 "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                 "USN: %s::urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                 "\r\n",
                 ssdp_timeout, local_ip.data(), http_port, ssdp_uuid.data());
        sendto(sock, response.data(), strlen(response.data()), 0, (sockaddr *)&client_addr, client_addr_len);
    }
}

void dlan::http_process()
{
    uint32_t sock = http_sock;
    sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    int32_t client_sock = accept(sock, (sockaddr *)&client_addr, &client_addr_len);
    if (client_sock < 0) {
        osal_printk("http接受连接失败\n");
        return;
    }

    static std::array<char, 2048> buffer; // HTTP接收缓冲区
    errcode_t ret = lwip_recv(client_sock, buffer.data()-1, buffer.size() - 1, 0);
    if (ret <= 0) {
        osal_printk("http数据接收失败\n");
        dlan_stop();
        return;
    }
    buffer[ret] = '\0';

    // 首先处理第一次连接的HTTP请求，解析配网信息并响应

}

void dlan::ssdp_ip_get()
{
    static netif *netif_p = netif_default;
    if (netif_p == nullptr || !netif_is_up(netif_p)) {
        snprintf(local_ip.data(), sizeof(local_ip), "0.0.0.0");
        osal_printk("获取默认网络接口失败\n");
        return;
    }

    snprintf(local_ip.data(), sizeof(local_ip), "%d.%d.%d.%d", (netif_p->ip_addr.u_addr.ip4.addr >> 0) & 0xFF,
             (netif_p->ip_addr.u_addr.ip4.addr >> 8) & 0xFF, (netif_p->ip_addr.u_addr.ip4.addr >> 16) & 0xFF,
             (netif_p->ip_addr.u_addr.ip4.addr >> 24) & 0xFF);
}

void dlan::dlan_stop()
{
    // 结束DLAN相关的套接字和资源
    if (ssdp_sock > 0) {
        lwip_close(ssdp_sock);
        ssdp_sock = 0;
    }
    if (http_sock > 0) {
        lwip_close(http_sock);
        http_sock = 0;
    }
}