#include "tcp.hpp"

namespace sed_ws63 {

tcp_client::~tcp_client()
{
    tcp_close();
}

errcode_t tcp_client::connect_host(const char *host, uint16_t port, uint32_t timeout_ms)
{
    // 先关闭之前的连接
    tcp_close();

    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        osal_printk("TCP socket创建失败\n");
        return 0x01; // 创建socket失败
    }

    // 设置连接超时
    timeval tv = {
        static_cast<long>(timeout_ms / 1000),
        static_cast<long>((timeout_ms % 1000) * 1000),
    };
    (void)lwip_setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)lwip_setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // tcp连接
    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    errcode_t ret = wifi_tool::get_ipv4_addr(host, &addr.sin_addr);
    if (ret != 0) {
        osal_printk("TCP主机解析失败: %s\n", host);
        tcp_close();
        return 0x02; // 主机解析失败
    }

    // 连接
    ret = lwip_connect(sfd, (sockaddr *)&addr, sizeof(addr));
    if (ret != 0) {
        osal_printk("TCP连接失败: %s:%u\n", host, port);
        tcp_close();
        return 0x03; // 连接失败
    }

    return ERRCODE_SUCC; // 成功
}

errcode_t tcp_client::send_data(const void *data, uint32_t len)
{
    if (sfd < 0 || data == nullptr || len == 0) {
        return 0x04;
    }

    const uint8_t *data_ptr = static_cast<const uint8_t *>(data);
    uint32_t total_sent = 0;
    while (total_sent < len) {
        int sent = lwip_send(sfd, data_ptr + total_sent, len - total_sent, 0);
        if (sent < 0) {
            osal_printk("TCP数据发送失败\n");
            return 0x05; // 数据发送失败
        }
        total_sent += sent;
    }
    return 100 + total_sent; // 成功，返回100+发送的字节数
}

errcode_t tcp_client::recv_data(void *buffer, uint32_t len, uint32_t timeout_ms)
{
    if (sfd < 0 || buffer == nullptr || len == 0) {
        return 0x04; // 参数错误
    }

    // 设置接收超时
    timeval tv = {
        static_cast<long>(timeout_ms / 1000),
        static_cast<long>((timeout_ms % 1000) * 1000),
    };
    lwip_setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int received = lwip_recv(sfd, buffer, len, 0);
    if (received < 0) {
        osal_printk("TCP数据接收失败\n");
        return 0x05; // 数据接收失败
    }
    return received; // 成功，返回接收的字节数
}

void tcp_client::tcp_close()
{
    if (sfd >= 0) {
        lwip_close(sfd);
        sfd = -1;
    }
}

tcp_listener::~tcp_listener()
{
    tcp_close();
}

errcode_t tcp_listener::start_listen(uint16_t port, const char *bind_ip, int backlog)
{
    // 先关闭之前的监听
    tcp_close();

    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        osal_printk("TCP监听socket创建失败\n");
        return 0x01; // 创建socket失败
    }

    int32_t reuse = 1;
    if (lwip_setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        osal_printk("TCP监听套接字设置SO_REUSEADDR失败\n");
    }

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(port);
    addr.sin_addr.s_addr = (bind_ip == nullptr) ? INADDR_ANY : inet_addr(bind_ip);

    if (lwip_bind(sfd, (sockaddr *)&addr, sizeof(addr)) != 0) {
        osal_printk("TCP监听套接字绑定失败\n");
        tcp_close();
        return 0x02; // 绑定失败
    }

    if (lwip_listen(sfd, backlog) != 0) {
        osal_printk("TCP监听套接字监听失败\n");
        tcp_close();
        return 0x03; // 监听失败
    }

    return ERRCODE_SUCC; // 成功
}

int32_t tcp_listener::accept_one(sockaddr_in *peer_addr)
{
    if (sfd < 0) {
        return -1; // 没有监听
    }

    sockaddr_in addr = {0};
    socklen_t addr_len = sizeof(addr);
    int32_t client_fd = lwip_accept(sfd, (sockaddr *)&addr, &addr_len);
    if (client_fd < 0) {
        osal_printk("TCP接受连接失败\n");
        return -1; // 接受连接失败
    }

    if (peer_addr != nullptr) {
        *peer_addr = addr; // 输出对端地址信息
    }
    return client_fd; // 返回新连接的socket文件描述符
}

void tcp_listener::tcp_close()
{
    if (sfd >= 0) {
        lwip_close(sfd);
        sfd = -1;
    }
}

} // namespace sed_ws63