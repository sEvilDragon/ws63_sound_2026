#pragma once
/*
    sed_ws63::wifi_tool.hpp
    该文件定义了一些处理html标签和其他网络相关的工具函数
*/

extern "C" {
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "soc_osal.h"
#include "errcode.h"
}
#include <array>
#include <string_view>
#include <cstring>
#include <iostream>
#include <cstdlib>

namespace sed_ws63 {

class wifi_tool {
public:
    class span_text {
    public:
        const char *ptr = nullptr;
        size_t len = 0;
    };
    class parse_url {
    public:
        std::array<char, 16> scheme = {0};
        std::array<char, 128> host = {0};
        std::array<char, 16> port_text = {0};
        std::array<char, 512> path = {0};
        std::array<char, 256> query = {0};
        std::array<char, 128> fragment = {0};
        uint16_t port = 0;
        bool has_explicit_port = false;
    };

private:
public:
    /*
        功能：
            安全地复制字符串，并确保目标字符串以'\0'结尾
        参数：
            dest：目标字符串缓冲区
            dest_size：目标字符串缓冲区大小
            src：源字符串
            src_size：源字符串大小（不包括'\0'）（默认使用strlen(src)）
        返回值：
            成功返回ERRCODE_SUCC，失败返回相应错误码
        错误码：
            ERRCODE_SUCC：成功
            0x01: 参数错误
            0x02: 目标缓冲区大小不足
    */
    static errcode_t copy_str(char *dest, size_t dest_size, const char *src, size_t src_size = 0);
    /*
        功能：
            去除前后空白字符
        错误码：
            ERRCODE_SUCC：成功
            0x01: 参数错误
    */
    static errcode_t trim(char *text);
    /*
        功能：
            检查字符串是否以指定的前后字符包围
        参数：
            text：输入字符串
            left: 需要检查的首字符
            right: 需要检查的尾字符
        返回值：
            成功返回ERRCODE_SUCC，失败返回相应错误码
        错误码：
            ERRCODE_SUCC：成功
            0x01: 参数错误
            0x02: 目标缓冲区大小不足
            0x03: 没有发现需要去除的字符
     */
    static errcode_t strip(char *text, char left, char right);
    /*
        功能：
            去除字符串前后字符，同时去除对应的首尾字符
        参数：
            text：输入字符串
            left: 需要去除的首字符
            right: 需要去除的尾字符
        返回值：
            成功返回true，失败返回false
    */
    static errcode_t trim_and_strip(char *text, char left, char right);
    /*
        功能：
            将字符中的所有大写字符转换为小写字符
        参数：
            text：输入字符
        返回值：
            返回对应的小写字符
    */
    static char to_lower(char ch);
    /*
        功能：
            比较字符串时忽略大小写
        参数：
            text1：输入字符串1
            text2：输入字符串2
        返回值：
            如果字符串相等（忽略大小写）返回true，否则返回false
    */
    static bool strcmp_ignore_case(const char *text1, const char *text2);
    /*
        功能：
            寻找字符串中是否存在指定的子字符串
            text：输入字符串
            substr：需要寻找的子字符串
        返回值：
            如果找到子字符串返回第一次出现的指针，否则返回nullptr
    */
    static const char *strstr_s(const char *text, const char *substr);
    /*
         功能：
             同前，但是不比较大小写
    */
    static const char *strstr_ignore_case(const char *text, const char *substr);
    /*
        功能：
            查询字符串是否包含对应字符串
        参数：
            text：输入字符串
            substr：需要寻找的子字符串
        返回值：
            如果找到子字符串返回true，否则返回false
    */
    static bool is_strstr_s(const char *text, const char *substr);
    /*
        功能：
            同前，但是不比较大小写
    */
    static bool is_strstr_ignore_case(const char *text, const char *substr);
    /*
        功能：
            找到对应http标签并返回标签中间内容
        参数：
            text：输入字符串
            tag: 需要寻找的标签
        返回值：
            返回span_text结构体，包含标签中间内容的地址和长度，如果未找到标签则ptr为nullptr，len为0
    */
    static const span_text find_html_tag_value(const char *text, const char *tag);
    /*
        功能：
            找到对应的http头并返回后面的值
        参数：
            text：输入的http请求字符串
            key: 需要寻找的http头
        返回值：
            返回span_text结构体，包含找到的值的地址和长度，如果未找到则ptr为nullptr，len为0
    */
    static const span_text find_http_header_value(const char *text, const char *key);
    /*
        功能：
            一次性得到tag标签中间内容，去除前后空白字符
        参数：
            text：输入字符串
            tag: 需要寻找的标签
        返回值：
            返回span_text结构体，包含标签中间内容的地址和长度，如果未找到标签则ptr为nullptr，len为0
    */
    static const span_text trim_and_find_tag_value(char *text, const char *tag);
    /*
        功能：
            去除字符串前后空白字符后，找到对应的http头并返回后面的值
        参数：
            text：输入的http请求字符串
            key: 需要寻找的http头
        返回值：
            返回span_text结构体，包含找到的值的地址和长度，如果未找到则ptr为nullptr，len为0
    */
    static const span_text trim_and_find_http_header_value(char *text, const char *key);
    /*
        功能：
            将字符串中的XML/HTML实体（如&amp;）转换为对应的字符（如&）
        参数：
            text：输入字符串
        返回值：
            成功返回ERRCODE_SUCC，失败返回相应错误码
        错误码：
            ERRCODE_SUCC：成功
            0x01: 参数错误
            0x03: 没有发现需要解码的字符
    */
    static errcode_t decode_xml_basic(char *text);
    /*
        功能：
            严格将字符串转换为整数
        参数：
            text：输入字符串
            answer：输出数字（类型由模板参数决定）
        返回值：
            成功返回ERRCODE_SUCC，失败返回相应错误码
        错误码：
            ERRCODE_SUCC：成功
            0x01: 参数错误
            0x04: 字符串格式错误
            0x05: 数字超出范围
    */
    static errcode_t strict_atoi(const char *text, auto &answer);
    /*
        功能：
            提取http请求中的URL路径部分
            只支持ipv4的URL格式
        参数：
            url：输入的完整URL字符串
            result：输出的解析结果结构体，包含URL的各个部分
        返回值：
            成功返回ERRCODE_SUCC，失败返回相应错误码
        错误码：
            ERRCODE_SUCC：成功
            0x01: 参数错误
            0x06：非法URL格式
            0x07: 缺失host
            0x08: host长度超过限制
            0x09: port缺失
            0x0A: port长度超过限制
            0x0B: port格式错误
            0x0C: 路径长度超过限制
    */
    static errcode_t get_url(const char *url, parse_url &result);
    /*
        功能：
            重塑parse_url的path部分，将其中的query和fragment部分分离出来
        参数：
            url_info：输入的URL解析结果结构体，包含URL的各个部分
        返回值：
            成功返回ERRCODE_SUCC，失败返回相应错误码
        错误码：
            ERRCODE_SUCC：成功
            0x01: 参数错误
            0x0D: URL格式错误，不存在路径部分或者不存在所需的分隔符
            0x0E: query部分长度超过限制
            0x0F: fragment部分长度超过限制
    */
    static errcode_t split_url_path(parse_url &url_info);
    /*
        功能：
            解析ipv4地址
        参数：
            host：输入的主机地址字符串，可以是点分十进制格式的IPv4地址或者域名
            out_addr：输出的解析结果，如果解析成功则包含对应的IPv4地址
        返回值：
            成功返回ERRCODE_SUCC，失败返回相应错误码
        错误码：
            ERRCODE_SUCC：成功
            0x01: 参数错误
            0x10: 无法解析地址
            0x11: 解析结果不是IPv4地址
    */
    static errcode_t get_ipv4_addr(const char *host, in_addr *out_addr);

private:
};

} // namespace sed_ws63