#include "udp.hpp"

namespace sed_ws63 {

errcode_t udp::bind_udp(uint16_t port, const char *bind_ip)
{
    // 创建udp套接字
    if (sfd >= 0) {
        return 0x01; // 套接字已创建
    }
    sfd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) {
        sfd = -1;
        return 0x02; // 创建套接字失败
    }

    sockaddr_in srv_addr = {0};                    // 服务器地址结构体
    srv_addr.sin_family = AF_INET;                 // 地址族设置为IPv4
    srv_addr.sin_addr.s_addr = inet_addr(bind_ip); // 设置服务地址
    srv_addr.sin_port = lwip_htons(port);          // 服务器端口

    // 绑定套接字
    if (lwip_bind(sfd, (sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        lwip_close(sfd);
        sfd = -1;
        return 0x03; // 绑定失败
    }

    return ERRCODE_SUCC; // 成功
}

errcode_t udp::receive_udp(uint8_t *buffer, uint32_t buffer_size, sockaddr_in *from_addr)
{
    if (sfd < 0) {
        return 0x01; // 套接字未创建
    }
    if (buffer_size == 0) {
        return 0x04; // 缓冲区大小为0
    }
    if (buffer == nullptr) {
        return 0x05; // 缓冲区指针为nullptr
    }

    sockaddr_in sender = {0};

    int32_t ret = lwip_recvfrom(sfd, buffer, buffer_size, 0, (sockaddr *)&sender, nullptr);
    if (ret < 0) {
        return 0x06; // 接收数据失败
    }
    buffer[ret] = '\0';

    if (from_addr != nullptr) {
        *from_addr = sender; // 将发送方地址信息存储到from_addr指向的结构体中
    }

    return 100 + ret; // 成功，返回接收到的数据大小
}

errcode_t udp::send_udp(const uint8_t *data, uint32_t data_size, const sockaddr_in &dest_addr)
{
    if (sfd < 0) {
        return 0x01; // 套接字未创建
    }
    if (data_size == 0) {
        return 0x04; // 数据大小为0
    }
    if (data == nullptr) {
        return 0x05; // 数据指针为nullptr
    }

    int32_t ret = lwip_sendto(sfd, data, data_size, 0, (const sockaddr *)&dest_addr, sizeof(dest_addr));
    if (ret < 0) {
        return 0x07; // 发送数据失败
    }

    return 100 + ret; // 成功，返回发送的数据大小
}

errcode_t udp::close_udp()
{
    if (sfd >= 0) {
        lwip_close(sfd);
        sfd = -1;
    }
    return ERRCODE_SUCC; // 成功
}

bool udp::is_udp_open() const
{
    return sfd >= 0; 
}

}