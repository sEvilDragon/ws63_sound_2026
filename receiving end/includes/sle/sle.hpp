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
    // �ṩһ���ӿ������ⲿ�������ݴ����ص�
    using data_process_t = void (*)(const uint8_t *data, uint16_t length);
    using data_clear_t = void (*)();
    static void set_data_process_fuction(data_process_t callback);
    static void set_data_clear_fuction(data_clear_t callback);

    static void teardown();
    static void reset_state();
    static volatile bool s_active;

private:
    // ���õ�ַ
    static void set_local_address();
    // ע��ssap�ͷ���
    static void set_mtu();
    static void set_ssap();
    static void set_service();
    static void set_property();
    static void service_start();
    // �㲥
    static void advertising_init();
    static void advertising_start();

    // ���ֻص�
    // ʹ�ܻص�
    static void sle_enable_callback(errcode_t status);
    // ����״̬�ı�Ļص�
    static void connect_changed_callback(uint16_t conn_id,
                                         const sle_addr_t *addr,
                                         sle_acb_state_t conn_state,
                                         sle_pair_state_t pair_state,
                                         sle_disc_reason_t disc_reason);
    // MTUЭ����ɺ�Ļص����������ʵ��Э�̵�MTUֵ
    static void ssap_mtu_callback(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status);
    // ������ݵĻص�����
    static void get_data_callback(uint8_t server_id,
                                  uint16_t conn_id,
                                  ssaps_req_write_cb_t *req_param,
                                  errcode_t status);

public:
    static uint16_t true_mtu;

private:
    // ����һ������ָ�����ͣ����ڽ������ݵĻص�
    static data_process_t data_process;
    static data_clear_t data_clear;

    // ���屾����ַ
    static constexpr uint8_t addr_type = 0;
    static constexpr std::array<uint8_t, 6> local_address = {0x20, 0x26, 0x03, 0x00, 0x11, 0xA1};
    static constexpr std::array<uint8_t, 6> peer_address = {0x20, 0x26, 0x03, 0x00, 0x11, 0x00};

    // ���Э��ֵ
    static constexpr uint16_t max_mtu = 800;

    // ���Ӷ�Ӧ��ʶ
    static uint8_t id; // ϵͳ�Զ������������
    static uint8_t conn_id;
    static uint16_t service_handle;  // ������
    static uint16_t property_handle; // ���Ծ��
    // ���uuid���ڱ�������
    static constexpr uint8_t uuid_user_1 = 0x20;
    static constexpr uint8_t uuid_user_2 = 0x25;
    static constexpr uint16_t uuid_service = 0x060B;
    static constexpr uint8_t uuid_service_audio_1 = 0x0B;
    static constexpr uint8_t uuid_service_audio_2 = 0x06;
    static constexpr uint16_t uuid_property = 0x060C;
    static constexpr uint8_t uuid_property_audio_1 = 0x0C;
    static constexpr uint8_t uuid_property_audio_2 = 0x06;

    // �ٶ���һЩ������Ȩ��
    static constexpr uint8_t property_permissions = SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE; // ��дȨ��
    static constexpr uint32_t operate_indication =
        SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE | SSAP_OPERATE_INDICATION_BIT_WRITE_NO_RSP;
    ;                                                   // ��дȨ�ޣ�д����ӦȨ��
    static constexpr uint16_t property_value_len = 0;   // ���Գ�ʼֵ���ȣ���Cԭʼ����һ�£�
    static constexpr uint8_t *property_value = nullptr; // ���Գ�ʼֵ

    // ������Ƶ����Ĺ㲥
    static constexpr uint8_t audio_announce_handle = 01; // �㲥���������ȷ�����ĸ��������ڹ㲥
    static constexpr uint16_t audio_announce_mode =
        SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;                                      // �㲥���ݰ����ͣ�������㲥
    static constexpr uint16_t audio_announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO; // �㲥��ɫ��Ŀ���Э��
    static constexpr uint16_t audio_announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;      // �㲥������ͨ
    static constexpr uint16_t audio_announce_channel_map = 0x7;                      // �㲥Ƶ��ӳ�䣬����Ƶ��
    static constexpr uint16_t audio_announce_interval_min = 0xC8;                    // �㲥�������λΪ����
    static constexpr uint16_t audio_announce_interval_max = 0xC8;                    // �㲥�������λΪ����
    static constexpr uint16_t audio_announce_tx_power = 20;
    // �㲥���Ӻ��Զ����¼��
    static constexpr uint16_t high_speed_interva_min = 0x14; // ��������Сֵ��SLEЭ����С�Ϸ�ֵ750?s��
    static constexpr uint16_t high_speed_interva_max = 0x14; // ���������ֵ��SLEЭ����С�Ϸ�ֵ750?s��
    static constexpr uint16_t high_speed_latency = 0x00;     // ����ʱ
    static constexpr uint16_t high_speed_timeout = 0x1F4;    // ��ʱ5000ms

    // ���ⷢ�͵Ĺ㲥���ݰ�
    static constexpr std::array<uint8_t, 7> advertising_data = {
        0x01,
        0x01,
        0x01, // �㲥���𣬺�����һ���ֽڣ�����ɷ���
        0x05,
        0x02, // ����16λ����UUID�б���������2���ֽ�
        uuid_service_audio_1,
        uuid_service_audio_2, // �����uuid
    };

    // ���ⷢ�͵�ɨ����Ӧ���ݰ�
    static constexpr std::array<uint8_t, 11> scan_response_data = {
        0x0B,
        0x09, // �����������ƣ�������9���ֽ�
        'S',  'E', 'D', '_', 'S', 'O', 'U', 'N',
        'D', // ��������Ϊ"SED_SOUND"
    };

    // ����PHY���
    static constexpr uint8_t phy_format = SLE_RADIO_FRAME_2;
    static constexpr uint8_t phy_phy = SLE_PHY_4M;
    static constexpr uint8_t phy_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1; // ��Ƶ���16
    static constexpr uint8_t phy_feedback = 0;
};

#endif