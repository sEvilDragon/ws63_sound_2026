#ifndef __SLE_HPP__
#define __SLE_HPP__
extern "C" {
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "common_def.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "sle_ssap_server.h"
#include "securec.h"
}

#include <array>
class sle {
public:
    sle();
    // 提供一个接口，给外部设置数据处理回调
    using data_process_t = void (*)(const uint8_t *data, uint16_t length);
    using data_clear_t = void (*)();
    static void set_data_process_fuction(data_process_t callback);
    static void set_data_clear_fuction(data_clear_t callback);

    static void teardown();
    static void reset_state();

    static volatile bool s_active;

private:
    // 设置本地地址
    static void set_local_address();

    // 注册ssap和服务
    static void set_mtu();
    static void set_ssap();
    static void set_service();
    static void set_property();
    static void service_start();

    // 广播
    static void advertising_init();
    static void advertising_start();

    // 各种回调
    // 使能回调
    static void sle_enable_callback(errcode_t status);
    // 连接状态改变的回调
    static void connect_changed_callback(uint16_t conn_id,
                                         const sle_addr_t *addr,
                                         sle_acb_state_t conn_state,
                                         sle_pair_state_t pair_state,
                                         sle_disc_reason_t disc_reason);

    // MTU协商完成后的回调，报告实际协商的MTU值
    static void ssap_mtu_callback(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status);

    // 接收数据的回调函数
    static void get_data_callback(uint8_t server_id,
                                  uint16_t conn_id,
                                  ssaps_req_write_cb_t *req_param,
                                  errcode_t status);

public:
    static uint16_t true_mtu;

private:
    // 定义一个函数指针类型，用于接收数据的回调
    static data_process_t data_process;
    static data_clear_t data_clear;

    // 定义本地地址
    static constexpr uint8_t addr_type = 0;
    static constexpr std::array<uint8_t, 6> local_address = {0x20, 0x26, 0x03, 0x00, 0x11, 0xA1};
    static constexpr std::array<uint8_t, 6> peer_address = {0x20, 0x26, 0x03, 0x00, 0x11, 0x00};

    // 最大协商值
    static constexpr uint16_t max_mtu = 800;

    // 连接对应标识
    static uint8_t id; // 系统自动分配，无需设置
    static uint8_t conn_id;
    static uint16_t service_handle;  // 服务句柄
    static uint16_t property_handle; // 属性句柄

    // 两个uuid，用于标识服务
    static constexpr uint8_t uuid_user_1 = 0x20;
    static constexpr uint8_t uuid_user_2 = 0x25;
    static constexpr uint16_t uuid_service = 0x060B;
    static constexpr uint8_t uuid_service_audio_1 = 0x0B;
    static constexpr uint8_t uuid_service_audio_2 = 0x06;
    static constexpr uint16_t uuid_property = 0x060C;
    static constexpr uint8_t uuid_property_audio_1 = 0x0C;
    static constexpr uint8_t uuid_property_audio_2 = 0x06;

    // 再定义一些属性和权限
    static constexpr uint8_t property_permissions = SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE; // 读写权限
    static constexpr uint32_t operate_indication = SSAP_OPERATE_INDICATION_BIT_READ |
                                                   SSAP_OPERATE_INDICATION_BIT_WRITE |
                                                   SSAP_OPERATE_INDICATION_BIT_WRITE_NO_RSP; // 读写权限，写不需要应答
    static constexpr uint16_t property_value_len = 0;   // 属性初始值长度，和C原始代码一致
    static constexpr uint8_t *property_value = nullptr; // 属性初始值

    // 用于传输音频的广播
    static constexpr uint8_t audio_announce_handle = 01; // 广播句柄，确保使用固定的句柄用于广播
    static constexpr uint16_t audio_announce_mode =
        SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;                                      // 广播数据包类型：可连接广播
    static constexpr uint16_t audio_announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO; // 广播角色：目标可协商
    static constexpr uint16_t audio_announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;      // 广播级别：普通
    static constexpr uint16_t audio_announce_channel_map = 0x7;                      // 广播频道映射：三个频道
    static constexpr uint16_t audio_announce_interval_min = 0xC8;                    // 广播间隔，单位为时隙
    static constexpr uint16_t audio_announce_interval_max = 0xC8;                    // 广播间隔，单位为时隙
    static constexpr uint16_t audio_announce_tx_power = 20;

    // 广播连接后自动更新参数
    static constexpr uint16_t high_speed_interva_min = 0x14; // 连接间隔最小值，SLE协议最小合法值750μs
    static constexpr uint16_t high_speed_interva_max = 0x14; // 连接间隔最大值，SLE协议最小合法值750μs
    static constexpr uint16_t high_speed_latency = 0x00;     // 延迟时间
    static constexpr uint16_t high_speed_timeout = 0x1F4;    // 超时5000ms

    // 额外发送的广播数据包
    static constexpr std::array<uint8_t, 7> advertising_data = {
        0x01,
        0x01,
        0x01, // 广播标志，含有一个字节，表示可发现
        0x05,
        0x02, // 包含16位服务UUID列表，后面有2个字节
        uuid_service_audio_1,
        uuid_service_audio_2, // 服务uuid
    };

    // 额外发送的扫描响应数据包
    static constexpr std::array<uint8_t, 11> scan_response_data = {
        0x0B,
        0x09, // 设备完整名称，长度为9个字节
        'S',  'E', 'D', '_', 'S', 'O', 'U', 'N',
        'D', // 设备名称为"SED_SOUND"
    };

    // 配置PHY参数
    static constexpr uint8_t phy_format = SLE_RADIO_FRAME_2;
    static constexpr uint8_t phy_phy = SLE_PHY_4M;
    static constexpr uint8_t phy_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1; // 导频密度16
    static constexpr uint8_t phy_feedback = 0;
};

#endif