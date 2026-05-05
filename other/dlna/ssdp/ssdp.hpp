#pragma once

/*
    sed_ws63:ssdp.hpp
    处理DLNA控制点SSDP请求的类，负责解析请求并调用相应的处理函数
*/

extern "C" {
#include "lwip/sockets.h"
#include "lwip/igmp.h"
#include "netinet/in.h"
#include "common_def.h"
#include "errcode.h"
}
#include "wifi_tool.hpp"

namespace sed_ws63 {

class ssdp {
public:
private:
    int32_t sfd = -1;
    // SSDP使用组播，所以不使用已有的简单udp封装
    static constexpr uint16_t ssdp_port = 1900;
    static constexpr const char *ssdp_multicast_addr = "239.255.255.250";
    static constexpr int32_t ssdp_max_age = 1800;
    static constexpr int32_t ssdp_boot_id = 1;
    static constexpr int32_t ssdp_config_id = 1;

public:
    ssdp() = default;
    ~ssdp();

    /*
        功能：
            打开SSDP套接字，加入组播组
        错误码：
            0x01 - 创建套接字失败
            0x02 - 设置套接字选项失败
            0x03 - 绑定套接字失败
            0x04 - 加入组播组失败
    */
    errcode_t open_socket();
    /*
        功能：
            处理一次SSDP请求，接收数据并解析请求内容
        参数：
            local_ip - 本地IP地址，用于过滤非本地请求
            http_port - HTTP服务器端口，用于构造响应中的LOCATION字段
            udn - 设备UDN，用于构造响应中的USN字段
        错误码：
            0x05 - 参数错误
            0x06 - 接收数据失败
            0x07 - 不是M-SEARCH请求，忽略
            0x08 - 请求缺少必要的ST、MAN或HOST头部
            0x09 - 复制ST头部值失败
            0x0A - 复制MAN头部值失败
            0x0B - 复制HOST头部值失败
            0x0C - MAN头部值不是"ssdp:discover"，忽略
            0x0D - HOST头部值不正确，忽略
    */
    errcode_t process_once(const char *local_ip, uint16_t http_port, const char *udn);
    /*
         功能：
            关闭SSDP套接字，离开组播组
     */
    void ssdp_close();
    /*
        功能：
            查看嵌套字
    */
    int32_t get_sfd() const
    {
        return sfd;
    }

private:
    /*
        功能：
            处理M-SEARCH请求，构造并发送响应
        参数：
            peer_addr - 请求来源地址，用于发送响应
            peer_len - 请求来源地址长度
            local_ip - 本地IP地址，用于构造响应中的LOCATION字段
            http_port - HTTP服务器端口，用于构造响应中的LOCATION字段
            udn - 设备UDN，用于构造响应中的USN字段
            st - M-SEARCH请求中的ST头部值，用于构造响应中的ST字段
            usn_suffix - USN字段的后缀部分，通常是设备类型或服务类型，用于构造响应中的USN字段，默认为空字符串
        错误码：
            0x0E - 发送响应失败
    */
    errcode_t send_reply(const sockaddr_in &peer_addr,
                         const socklen_t peer_len,
                         const char *local_ip,
                         uint16_t http_port,
                         const char *udn,
                         const char *st,
                         const char *usn_suffix = nullptr);
};
} // namespace sed_ws63