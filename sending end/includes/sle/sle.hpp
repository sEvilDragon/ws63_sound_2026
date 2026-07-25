#ifndef __SLE_HPP__
#define __SLE_HPP__

/*
    该星闪为单设备连接的发送连接段
*/

extern "C" {
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "sle_ssap_client.h"
#include "common_def.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "osal_semaphore.h"
#include "nv.h"
}
#include <array>

// 声明类内未包含的mcs函数
// 这些函数用于实现高速phy通信
extern errcode_t sle_set_mcs(uint16_t conn_id, uint8_t mcs);
extern errcode_t sle_set_data_len(uint16_t conn_id, uint16_t len);

class sle {
public:
    sle();
    // 第一个参数是连接设备的索引，第二个参数是要发送的数据，第三个参数是数据长度（字节为单位）
    static void write_send(int index, uint8_t *data, uint16_t len_b);
    static bool compression_enabled();
    static bool mono_enabled();
    static void request_audio_state();

private:
    // SLE使能回调函数，SLE使能成功后会调用该函数
    static void sle_enable_callback(errcode_t status);
    // 下面定义的是设备发现
    static void set_local_address();
    // 启动扫描
    static void start_seek();
    // 发现设备后调用的函数：保存地址并停止扫描
    static void seek_finded_callback(sle_seek_result_info_t *seek_result_data);
    // 扫描停止后调用的函数：真正发起连接
    static void seek_disable_callback(errcode_t status);
    // 寻找已连接设备中conn_id对应的设备
    static int find_connectioned_device_connid(uint16_t conn_id);
    // 寻找正在连接但不活跃的设备槽位，即连接被拒绝的设备槽位
    static int find_pending_butnot_active_device();
    // 寻找是否还有空闲的连接设备槽位
    static int find_free_connection_device_conned();
    // 在连接状态发生改变时调用
    static void connect_changed_callback(uint16_t conn_id,
                                         const sle_addr_t *addr,
                                         sle_acb_state_t conn_state,
                                         sle_pair_state_t pair_state,
                                         sle_disc_reason_t disc_reason);
    // 配置服务
    static void find_service(int index);
    // 发现服务的回调
    static void find_service_callback(uint8_t client_id,
                                      uint16_t conn_id,
                                      ssapc_find_service_result_t *svc,
                                      errcode_t status);
    // 发现特征的回调
    static void find_property_callback(uint8_t client_id,
                                       uint16_t conn_id,
                                       ssapc_find_property_result_t *property,
                                       errcode_t status);
    // 配置SSAP连接函数
    static void ssap_connect(uint16_t conn_id);
    // MTU协商完成后的回调函数，配置数据长度
    static void ssap_mtu_callback(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status);
    static void notification_callback(uint8_t client_id,
                                      uint16_t conn_id,
                                      ssapc_handle_value_t *data,
                                      errcode_t status);
    static void write_confirm_callback(uint8_t client_id,
                                       uint16_t conn_id,
                                       ssapc_write_result_t *write_result,
                                       errcode_t status);
    // 在PHY设置后，调用回调函数，配置mcs
    static void after_phy_set_callback(uint16_t conn_id, errcode_t status, const sle_set_phy_t *param);

public:
    // 定义一些基础值
    // 定义一个连接设备的结构体，保存连接设备的信息
    class connection_device {
    public:
        uint16_t conn_id = -1;      // 链路层连接ID (由协议栈分配)
        uint8_t client_id = 0;      // 应用层客户端ID (由 ssapc_register_client 返回)
        uint16_t target_handle = 0; // 目标特征句柄 (用于发送数据)
        uint8_t device_type = 0;    // 设备类型
        sle_addr_t peer_addr = {0}; // 对端设备MAC地址
        bool is_active = false;     // 连接是否活跃
        bool is_pending = false;    // 连接发起中（已调用 sle_connect_remote_device 但尚未收到 CONNECTED 回调）
        uint16_t mtu = 0;           // MTU大小
    };

    static constexpr int max_connection_num = 1;                                 // 最大连接数为1
    static std::array<connection_device, max_connection_num> connection_devices; // 连接设备列表

private:
    // 定义NV发射功率的索引和默认值
    static constexpr uint8_t nv_tx = 7; // 最大发射功率，改善 MCS12 在非理想信道下的稳定性
    static constexpr uint16_t nv_key = 0x20A0;

    // 定义本机地址
    static constexpr uint8_t addr_type = 0;
    static constexpr std::array<uint8_t, 6> local_address = {0x20, 0x26, 0x03, 0x00, 0x11, 0x00};
    static constexpr std::array<uint8_t, 6> peer_address = {0x20, 0x26, 0x03, 0x00, 0x11, 0xA1};

    // 下面定义的是扫描时的参数
    static constexpr uint8_t own_addr_type = 0;      // 使用公共地址
    static constexpr uint8_t filter_duplicates = 0;  // 与官方示例一致，关闭重复过滤
    static constexpr uint8_t seek_filter_policy = 0; // 海思没有白名单sdk
    static constexpr uint8_t seek_phys = 0x01;       // 扫描1M PHY
    static constexpr uint8_t seek_type = 0;          // 被动扫描（官方示例使用0）
    static constexpr uint16_t seek_interval = 100;
    static constexpr uint16_t seek_window = 100;

    static constexpr uint8_t enable_filter_policy = 0; // 海思没有白名单sdk
    static constexpr uint8_t enable_phys = 0x01;       // 连接1M PHY
    static constexpr uint8_t gt_negotiate = 1;         // 协商
    static constexpr uint16_t scan_interval = 400;
    static constexpr uint16_t scan_window = 20;
    static constexpr uint16_t scan_interval_min = 0x0014;
    static constexpr uint16_t scan_interval_max = 0x0014;
    static constexpr uint16_t scan_timeout = 0x1F4;

    static sle_addr_t s_pending_addr; // 正在被连接的设备地址，保存到连接回调中使用
    static volatile bool s_adpcm_enabled;
    static volatile bool s_mono_enabled;
    static volatile bool s_audio_state_valid;

    // 下面定义一些连接到设备后，需要开放的高速配置
    static constexpr uint16_t high_speed_interva_min = 0x14; // 传输间隔最小值
    static constexpr uint16_t high_speed_interva_max = 0x14; // 传输间隔最大值
    static constexpr uint16_t high_speed_latency = 0x00;     // 无延时
    static constexpr uint16_t high_speed_timeout = 0x1F4;    // 超时5000ms
    // 调整PHY，开放4M高速通道
    static constexpr uint8_t format = SLE_RADIO_FRAME_2; // 开放4M通道
    static constexpr uint8_t phy = SLE_PHY_4M;           // 无特殊选项
    static constexpr uint8_t pilot_density =
        SLE_PHY_PILOT_DENSITY_16_TO_1;          // 与接收端保持一致，恢复旧版本配置
    static constexpr uint8_t feedback = 0; // 关闭反馈机制，增加传输效率

    // 设置mcs
    static constexpr uint8_t uuid_user_1 = 0x20;
    static constexpr uint8_t uuid_user_2 = 0x25;
    static constexpr uint16_t uuid_service = 0x060B;
    static constexpr uint8_t uuid_service_audio_1 = 0x0B;
    static constexpr uint8_t uuid_service_audio_2 = 0x06;
    static constexpr uint16_t uuid_property = 0x060C;
    static constexpr uint8_t uuid_property_audio_1 = 0x0C;
    static constexpr uint8_t uuid_property_audio_2 = 0x06;
    static constexpr uint8_t mcs = 12;
    static constexpr int mtu_max = 800;                                          // 800
    static constexpr uint8_t find_service_type = SSAP_FIND_TYPE_PRIMARY_SERVICE; // 查找最大范围内的服务
    static constexpr uint8_t find_property_type = SSAP_FIND_TYPE_PROPERTY;       // 查找最大范围内的特征
    static constexpr uint8_t find_write_type = SSAP_PROPERTY_TYPE_VALUE;         // 写数据到特征值
    static constexpr uint16_t start_hdl = 0x0001;                                // 起始句柄
    static constexpr uint16_t end_hdl = 0xFFFF;                                  // 结束句柄
};

#endif
