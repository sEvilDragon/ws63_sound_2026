#pragma once

/*
    sed_ws63:event.hpp
    处理subscription事件的类，包含事件的类型和事件的内容
*/

#include "dlna_types/dlna_types.hpp"
#include "wifi_tool.hpp"
#include "tcp.hpp"

namespace sed_ws63 {
class subscription_manager {
public:
private:
    dlna_subscribe_slot avtransport_;
    dlna_subscribe_slot rendering_control_;
    dlna_subscribe_slot connection_manager_;

public:
    subscription_manager();
    ~subscription_manager() = default;

    /*
        功能：清除指定服务类型的订阅信息
        参数：
            kind: 服务类型，如dlna_service_kind::avtransport
    */
    void clear(dlna_service_kind kind);
    /*
        功能：更新或插入订阅信息
        参数：
            kind: 服务类型，如dlna_service_kind::avtransport
            sid: 订阅ID
            callback: 回调URL
            timeout_sec: 超时时间（秒）
    */
    void upsert(dlna_service_kind kind, const char *sid, const char *callback, uint32_t timeout_sec = 1800);
    /*
        功能：获取下一个序列号
        参数：
            kind: 服务类型，如dlna_service_kind::avtransport
        返回值：下一个序列号
    */
    uint32_t next_seq(dlna_service_kind kind);
    /*
        功能：获取订阅槽
        参数：
            kind: 服务类型，如dlna_service_kind::avtransport
        返回值：订阅槽指针
    */
    dlna_subscribe_slot *get(dlna_service_kind kind);
    /*
        功能：获取订阅槽（const版本）
        参数：
            kind: 服务类型，如dlna_service_kind::avtransport
        返回值：订阅槽指针（只读）
    */
    const dlna_subscribe_slot *get(dlna_service_kind kind) const;

private:
};

class notify_sender {
public:
private:
public:
    /*
        功能：发送事件通知
        参数：
            slot: 订阅槽，包含订阅信息
            seq: 序列号
            body: 事件内容
        错误码：
            0x01: 无效的订阅槽
            0x02: 无效的URL
    */
    errcode_t send_event(const dlna_subscribe_slot &slot, uint32_t seq, const char *body);

private:
};
} // namespace sed_ws63