#pragma once

extern "C" {
#include "lwip/sockets.h"
#include "soc_osal.h"
#include "lwip/netifapi.h"
#include "netinet/in.h"
#include "common_def.h"
#include "errcode.h"
}

/*
    sed_ws63::udp.hpp
    为网络添加可扩展的udp功能
*/

namespace sed_ws63 {

class udp {
public:
private:
    int32_t sfd = -1; // UDP套接字文件描述符，初始值为-1表示未创建
public:
    udp() = default;
    ~udp()
    {
        close_udp();
    } // 析构函数，确保对象销毁时关闭UDP套接字;
    /*
        功能：创建UDP套接字并绑定到指定端口，准备接收数据
        参数：port - 监听的UDP端口号
              bind_ip - 绑定的IP地址
        返回值：ERRCODE_SUCC表示成功，ERRCODE_FAIL表示失败(失败会自动关闭套接字)
        错误码：
            0x01 - 套接字已创建
            0x02 - 创建套接字失败
            0x03 - 绑定失败
    */
    errcode_t bind_udp(uint16_t port, const char *bind_ip);
    /*
        功能：接收UDP数据并处理
        参数：buffer - 用于存储接收到的数据的缓冲区
              buffer_size - 缓冲区的大小
              from_addr - 用于存储发送方地址信息的结构体指针(默认nullptr表示不需要发送方信息)
        返回值：100 + 接收到的数据大小，小于100表示对应的错误码
        错误码：
            0x01 - 套接字未创建
            0x04 - 缓冲区大小为0
            0x05 - 缓冲区指针为nullptr
            0x06 - 接收数据失败
    */
    errcode_t receive_udp(uint8_t *buffer, uint32_t buffer_size, sockaddr_in *from_addr = nullptr);
    /*
        功能：发送UDP数据
        参数：data - 要发送的数据缓冲区
              data_size - 数据的大小
              dest_addr - 目标地址结构体，包含IP和端口信息
        返回值：100 + 发送的数据大小，小于100表示对应的错误码
        错误码：
            0x01 - 套接字未创建
            0x04 - 数据大小为0
            0x05 - 数据指针为nullptr
            0x07` - 发送数据失败
    */
    errcode_t send_udp(const uint8_t *data, uint32_t data_size, const sockaddr_in &dest_addr);
    /*
        功能：关闭UDP套接字，释放资源
        返回值：ERRCODE_SUCC表示成功，ERRCODE_FAIL表示失败
    */
    errcode_t close_udp();
    /*
        功能：查询UDP套接字是否处于打开状态
        返回值：true表示套接字已打开，false表示套接字未打开
    */
    bool is_udp_open() const;

private:
};

} // namespace sed_ws63