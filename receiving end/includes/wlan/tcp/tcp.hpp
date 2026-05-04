#pragma once

/*
    sed_ws63:tcp.hpp
    tcp服务的实现，tcp分为client和listener两个类
*/

extern "C" {
#include "lwip/sockets.h"
#include "netinet/in.h"
#include "common_def.h"
#include "soc_osal.h"
#include "errcode.h"
}
#include "wifi_tool.hpp"

namespace sed_ws63 {

class tcp_client {
public:
private:
    int32_t sfd = -1; // socket文件描述符
public:
    tcp_client() = default;
    ~tcp_client();

    /*
        功能：连接到指定host和port的服务器
        参数：
            host: 服务器地址，可以是域名或IP地址
            port: 服务器端口
            timeout_ms: 连接超时时间，单位毫秒，默认10000ms
        错误码：
            0x01: 创建socket失败
            0x02: 主机解析失败
            0x03: 连接失败
    */
    errcode_t connect_host(const char *host, uint16_t port, uint32_t timeout_ms = 10000);
    /*
        功能：发送tcp数据
        参数：
            data: 待发送数据的指针
            len: 待发送数据的长度
        错误码：
            如果成功，返回100+发送的字节数
            0x04: 参数错误
            0x05: 发送失败
    */
    errcode_t send_data(const void *data, uint32_t len);
    /*
        功能：接收tcp数据
        参数：
            buffer: 接收数据的缓冲区指针
            len: 待接收数据的最大长度
            timeout_ms: 接收超时时间，单位毫秒，默认10000ms
        错误码：
            如果成功，返回100+接收的字节数
            0x04: 参数错误
            0x06: 接收失败
    */
    errcode_t recv_data(void *buffer, uint32_t len, uint32_t timeout_ms = 10000);
    /*
        功能：关闭tcp连接
    */
    void tcp_close();
    /*
        功能：检查是否有连接存在
    */
    bool is_connected() const
    {
        return sfd >= 0;
    }

private:
};

class tcp_listener {
public:
private:
    int32_t sfd = -1; // socket文件描述符
public:
    tcp_listener() = default;
    ~tcp_listener();
    /*
        功能：建立监听，默认绑定所有地址并允许少量排队
        参数：
            port: 监听端口
            bind_ip: 绑定的IP地址，默认为nullptr表示绑定所有地址
            backlog: listen函数的backlog参数，默认为2
        错误码：   
            0x01: 创建socket失败
            0x02: 绑定失败
            0x03: 监听失败
    */
    errcode_t start_listen(uint16_t port, const char *bind_ip = nullptr, int backlog = 2);
    /*
        功能：接受一个连接，返回新连接的socket文件描述符，失败返回-1
        参数：
            peer_addr: 输出参数，接收连接的对端地址信息，默认nullptr表示不获取对端地址
        返回值：
            返回新连接的socket文件描述符，失败返回-1
    */
   int32_t accept_one(sockaddr_in *peer_addr = nullptr);
    /*
        功能：关闭tcp监听
    */
    void tcp_close();
    /*
        功能：检查是否有监听存在
    */
    bool is_listening() const
    {
        return sfd >= 0;
    }
    /*
        功能：返回sfd，供外部调用accept等函数使用
    */
    int32_t get_sfd() const
    {
        return sfd;
    }

private:
};

} // namespace sed_ws63