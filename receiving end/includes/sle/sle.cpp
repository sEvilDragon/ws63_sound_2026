#include "sle.hpp"
#include "../../../other/adpcm/adpcm.hpp"
#include "nv_recv.hpp"

uint16_t sle::true_mtu = 0;
uint8_t sle::id = 0;
uint16_t sle::conn_id = 0;
bool sle::s_connected = false;
uint16_t sle::service_handle = 0;
uint16_t sle::property_handle = 0;
sle::data_process_t sle::data_process = nullptr;
sle::data_clear_t sle::data_clear = nullptr;
volatile bool sle::s_active = false;

void sle::set_data_process_fuction(data_process_t callback)
{
    data_process = callback;
}

void sle::set_data_clear_fuction(data_clear_t callback)
{
    data_clear = callback;
}

bool sle::adpcm_enabled()
{
    return nv_recv_sle_adpcm_enabled() != 0;
}

bool sle::set_adpcm_enabled(bool enabled)
{
    if (!nv_recv_write_sle_adpcm(enabled ? 1 : 0)) {
        return false;
    }
    notify_adpcm_state();
    return true;
}

void sle::notify_adpcm_state()
{
    if (!s_active || !s_connected || property_handle == 0) {
        return;
    }

    uint8_t value[sle_audio::codec_control_size] = {
        sle_audio::codec_control_magic,
        sle_audio::codec_control_version,
        static_cast<uint8_t>(adpcm_enabled() ? 1 : 0),
    };
    ssaps_ntf_ind_t param = {0};
    param.handle = property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value_len = sizeof(value);
    param.value = value;
    errcode_t ret = ssaps_notify_indicate(id, conn_id, &param);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] ADPCM state notify failed: %u\r\n", ret);
    } else {
        osal_printk("[SLE] ADPCM state notified: %s\r\n", adpcm_enabled() ? "ON" : "OFF");
    }
}

sle::sle()
{
    s_active = true;

    sle_announce_seek_callbacks_t a = {0};
    a.sle_enable_cb = sle_enable_callback;

    sle_connection_callbacks_t b = {0};
    b.connect_state_changed_cb = connect_changed_callback;

    ssaps_callbacks_t c = {0};
    c.mtu_changed_cb = ssap_mtu_callback;
    c.write_request_cb = get_data_callback;

    sle_announce_seek_register_callbacks(&a);
    sle_connection_register_callbacks(&b);
    ssaps_register_callbacks(&c);

    errcode_t ret = enable_sle();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] enable failed: %u\n", ret);
    }
}

void sle::teardown()
{
    s_active = false;
    osal_printk("[SLE] teardown start\r\n");

    sle_stop_announce(audio_announce_handle);
    sle_disconnect_all_remote_device();
    if (id != 0) {
        ssaps_delete_all_services(id);
        ssaps_unregister_server(id);
    }
    sle_remove_announce(audio_announce_handle);
    disable_sle();

    osal_msleep(200);
    reset_state();
    osal_printk("[SLE] teardown done\r\n");
}

void sle::reset_state()
{
    id = 0;
    conn_id = 0;
    s_connected = false;
    service_handle = 0;
    property_handle = 0;
    true_mtu = 0;
}

void sle::set_local_address()
{
    static sle_addr_t local_addr;
    local_addr.type = addr_type;
    local_addr.addr[0] = local_address[0];
    local_addr.addr[1] = local_address[1];
    local_addr.addr[2] = local_address[2];
    local_addr.addr[3] = local_address[3];
    local_addr.addr[4] = local_address[4];
    local_addr.addr[5] = local_address[5];
    errcode_t ret = sle_set_local_addr(&local_addr);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] set local addr failed: %u\n", ret);
    }
}

void sle::set_mtu()
{
    ssap_exchange_info_t info = {0};
    info.mtu_size = max_mtu;
    info.version = 1;
    errcode_t ret = ssaps_set_info(id, &info);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] set MTU failed: %u\n", ret);
    }
}

void sle::set_ssap()
{
    sle_uuid_t ssap_uuid = {0};
    ssap_uuid.len = 2;
    ssap_uuid.uuid[0] = uuid_user_1;
    ssap_uuid.uuid[1] = uuid_user_2;
    errcode_t ret = ssaps_register_server(&ssap_uuid, &id);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] register server failed: %u\n", ret);
    }
}

void sle::set_service()
{
    sle_uuid_t service_uuid = {0};
    service_uuid.len = 2;
    service_uuid.uuid[0] = uuid_service_audio_1;
    service_uuid.uuid[1] = uuid_service_audio_2;
    errcode_t ret = ssaps_add_service_sync(id, &service_uuid, true, &service_handle);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] add service failed: %u\n", ret);
    }
}

void sle::set_property()
{
    ssaps_property_info_t property_info = {0};
    property_info.uuid.len = 2;
    property_info.uuid.uuid[0] = uuid_property_audio_1;
    property_info.uuid.uuid[1] = uuid_property_audio_2;
    property_info.permissions = property_permissions;
    property_info.operate_indication = operate_indication;
    property_info.value_len = 0;
    property_info.value = nullptr;

    errcode_t ret = ssaps_add_property_sync(id, service_handle, &property_info, &property_handle);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] add property failed: %u\n", ret);
        return;
    }

    uint8_t notify_enabled[] = {0x01, 0x00};
    ssaps_desc_info_t descriptor = {0};
    descriptor.permissions = SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE;
    descriptor.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    descriptor.type = SSAP_DESCRIPTOR_CLIENT_CONFIGURATION;
    descriptor.value_len = sizeof(notify_enabled);
    descriptor.value = notify_enabled;
    ret = ssaps_add_descriptor_sync(id, service_handle, property_handle, &descriptor);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] add notify descriptor failed: %u\n", ret);
    }
}

void sle::service_start()
{
    errcode_t ret = ssaps_start_service(id, service_handle);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] start service failed: %u\n", ret);
    }
    osal_printk("[SLE] SSAP service started\r\n");
}

void sle::advertising_init()
{
    sle_announce_param_t advertising_param = {0};
    advertising_param.announce_handle = audio_announce_handle;
    advertising_param.announce_mode = audio_announce_mode;
    advertising_param.announce_gt_role = audio_announce_gt_role;
    advertising_param.announce_level = audio_announce_level;
    advertising_param.announce_interval_min = audio_announce_interval_min;
    advertising_param.announce_interval_max = audio_announce_interval_max;
    advertising_param.announce_channel_map = audio_announce_channel_map;
    advertising_param.announce_tx_power = audio_announce_tx_power;
    advertising_param.own_addr.type = addr_type;
    advertising_param.own_addr.addr[0] = local_address[0];
    advertising_param.own_addr.addr[1] = local_address[1];
    advertising_param.own_addr.addr[2] = local_address[2];
    advertising_param.own_addr.addr[3] = local_address[3];
    advertising_param.own_addr.addr[4] = local_address[4];
    advertising_param.own_addr.addr[5] = local_address[5];
    advertising_param.conn_interval_min = high_speed_interva_min;
    advertising_param.conn_interval_max = high_speed_interva_max;
    advertising_param.conn_max_latency = high_speed_latency;
    advertising_param.conn_supervision_timeout = high_speed_timeout;

    sle_remove_announce(audio_announce_handle);

    errcode_t ret1 = sle_set_announce_param(audio_announce_handle, &advertising_param);
    if (ret1 != ERRCODE_SUCC) {
        osal_printk("[SLE] set announce param failed: %u\n", ret1);
    }

    sle_announce_data_t announce_data = {0};
    announce_data.announce_data = (uint8_t *)(advertising_data.data());
    announce_data.announce_data_len = advertising_data.size();
    announce_data.seek_rsp_data = (uint8_t *)(scan_response_data.data());
    announce_data.seek_rsp_data_len = scan_response_data.size();

    errcode_t ret2 = sle_set_announce_data(audio_announce_handle, &announce_data);
    if (ret2 != ERRCODE_SUCC) {
        osal_printk("[SLE] set announce data failed: %u\n", ret2);
    }
}

void sle::advertising_start()
{
    errcode_t ret = sle_start_announce(audio_announce_handle);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SLE] start announce failed: %u\n", ret);
    }
}

void sle::sle_enable_callback(errcode_t status)
{
    if (status != ERRCODE_SUCC) {
        osal_printk("[SLE] enable callback failed: %u\n", status);
        return;
    }

    set_local_address();
    set_ssap();
    set_service();
    set_property();
    set_mtu();
    service_start();
    advertising_init();
    advertising_start();
}

void sle::connect_changed_callback(uint16_t conn_id,
                                   const sle_addr_t *addr,
                                   sle_acb_state_t conn_state,
                                   sle_pair_state_t pair_state,
                                   sle_disc_reason_t disc_reason)
{
    unused(pair_state);
    unused(disc_reason);
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        osal_printk("[SLE] connected, conn_id=%u\r\n", conn_id);
        sle::conn_id = conn_id;
        s_connected = true;

        sle_stop_announce(audio_announce_handle);

        sle_connection_param_update_t param_update = {0};
        param_update.conn_id = conn_id;
        param_update.interval_min = high_speed_interva_min;
        param_update.interval_max = high_speed_interva_max;
        param_update.max_latency = high_speed_latency;
        param_update.supervision_timeout = high_speed_timeout;
        errcode_t ret = sle_update_connect_param(&param_update);
        if (ret != ERRCODE_SUCC) {
            osal_printk("[SLE] connect param update failed: %u\n", ret);
        }

        sle_set_phy_t phy_param = {0};
        phy_param.tx_format = phy_format;
        phy_param.rx_format = phy_format;
        phy_param.tx_phy = phy_phy;
        phy_param.rx_phy = phy_phy;
        phy_param.tx_pilot_density = phy_pilot_density;
        phy_param.rx_pilot_density = phy_pilot_density;
        phy_param.g_feedback = phy_feedback;
        phy_param.t_feedback = phy_feedback;
        errcode_t ret_phy = sle_set_phy_param(conn_id, &phy_param);
        if (ret_phy != ERRCODE_SUCC) {
            osal_printk("[SLE] set PHY failed: %u\n", ret_phy);
        }

        /* Try immediately; MTU callback repeats this after SSAP is ready. */
        notify_adpcm_state();

    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        osal_printk("[SLE] disconnected, conn_id=%u\r\n", conn_id);
        sle::conn_id = 0;
        s_connected = false;

        if (data_clear != nullptr && s_active) {
            data_clear();
        }

        if (!s_active)
            return;

        set_mtu();
        advertising_init();
        advertising_start();
    }
}

void sle::ssap_mtu_callback(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SUCC) {
        osal_printk("[SLE] MTU negotiate failed: %u\n", status);
        return;
    }
    true_mtu = param->mtu_size;
    notify_adpcm_state();
}

void sle::get_data_callback(uint8_t server_id, uint16_t conn_id, ssaps_req_write_cb_t *req_param, errcode_t status)
{
    unused(server_id);
    if (status != ERRCODE_SUCC) {
        osal_printk("[SLE] write callback failed: %u\n", status);
        return;
    }
    static int gdc_cnt = 0;
    gdc_cnt++;
    if (gdc_cnt <= 3 || gdc_cnt % 50 == 1) {
    }
    if (!s_active)
        return;
    if (req_param == nullptr || req_param->handle != property_handle ||
        req_param->type != SSAP_PROPERTY_TYPE_VALUE) {
        return;
    }
    if (data_process == nullptr) {
        osal_printk("[SLE] data_process not init\r\n");
        return;
    }
    data_process(req_param->value, req_param->length);
}
