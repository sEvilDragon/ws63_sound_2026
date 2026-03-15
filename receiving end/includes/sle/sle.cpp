#include "sle.hpp"
uint16_t sle::true_mtu = 0;
uint8_t sle::id = 0;
uint8_t sle::conn_id = 0;
uint16_t sle::service_handle = 0;
uint16_t sle::property_handle = 0;
sle::data_process_t sle::data_process = nullptr;
sle::data_clear_t sle::data_clear = nullptr;

void sle::set_data_process_fuction(data_process_t callback)
{
    data_process = callback;
}

void sle::set_data_clear_fuction(data_clear_t callback)
{
    data_clear = callback;
}

sle::sle()
{
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
        osal_printk("SLE使能失败，错误码：%u\n", ret);
    }
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
        osal_printk("SLE本地地址设置失败，错误码：%u\n", ret);
    }
}

void sle::set_mtu()
{
    // 设置MTU
    ssap_exchange_info_t info = {0};
    info.mtu_size = max_mtu;
    info.version = 1;
    errcode_t ret = ssaps_set_info(id, &info);
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE MTU设置失败，错误码：%u\n", ret);
    }
}

void sle::set_ssap()
{
    // 设置SSAP
    sle_uuid_t ssap_uuid = {0};
    ssap_uuid.len = 2;
    ssap_uuid.uuid[0] = uuid_user_1;
    ssap_uuid.uuid[1] = uuid_user_2;
    errcode_t ret = ssaps_register_server(&ssap_uuid, &id);
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE SSAP设置失败，错误码：%u\n", ret);
    }
}

void sle::set_service()
{
    // 设置服务
    sle_uuid_t service_uuid = {0};
    service_uuid.len = 2;
    service_uuid.uuid[0] = uuid_service_audio_1;
    service_uuid.uuid[1] = uuid_service_audio_2;
    errcode_t ret = ssaps_add_service_sync(id, &service_uuid, true, &service_handle);
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE服务设置失败，错误码：%u\n", ret);
    }
}

void sle::set_property()
{
    // 设置属性
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
        osal_printk("SLE属性设置失败，错误码：%u\n", ret);
    }
}

void sle::service_start()
{
    // 启动服务
    errcode_t ret = ssaps_start_service(id, service_handle);
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE服务启动失败，错误码：%u\n", ret);
    }

    osal_printk("SSAP服务启动成功\n");
}

void sle::advertising_init()
{
    // 广播初始化
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
    // peer_addr 不设置：CONNECTABLE_SCANABLE 是非定向广播，peer_addr 必须全零
    advertising_param.conn_interval_min = high_speed_interva_min;
    advertising_param.conn_interval_max = high_speed_interva_max;
    advertising_param.conn_max_latency = high_speed_latency;
    advertising_param.conn_supervision_timeout = high_speed_timeout;

    // 先移除可能存在的旧广播配置（与原C代码保持一致）
    sle_remove_announce(audio_announce_handle);

    // 添加新广播
    errcode_t ret1 = sle_set_announce_param(audio_announce_handle, &advertising_param);
    if (ret1 != ERRCODE_SUCC) {
        osal_printk("SLE广播初始化失败，错误码：%u\n", ret1);
    }

    sle_announce_data_t announce_data = {0};
    announce_data.announce_data = (uint8_t *)(advertising_data.data());
    announce_data.announce_data_len = advertising_data.size();
    announce_data.seek_rsp_data = (uint8_t *)(scan_response_data.data());
    announce_data.seek_rsp_data_len = scan_response_data.size();

    errcode_t ret2 = sle_set_announce_data(audio_announce_handle, &announce_data);
    if (ret2 != ERRCODE_SUCC) {
        osal_printk("SLE广播数据设置失败，错误码：%u\n", ret2);
    }
}

void sle::advertising_start()
{
    // 启动广播
    errcode_t ret = sle_start_announce(audio_announce_handle);
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE广播启动失败，错误码：%u\n", ret);
    }
}

void sle::sle_enable_callback(errcode_t status)
{
    if (status != ERRCODE_SUCC) {
        osal_printk("SLE使能失败，错误码：%u\n", status);
        return;
    }

    // 设置地址
    set_local_address();

    // 注册SSAP和服务
    set_ssap();
    set_service();
    set_property();

    set_mtu();

    // 启动服务
    service_start();

    // 广播初始化
    advertising_init();
    // 启动广播
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
        osal_printk("SLE已连接，连接ID：%u\n", conn_id);
        sle::conn_id = conn_id; // 保存当前连接ID

        sle_stop_announce(audio_announce_handle);

        // 更新连接
        sle_connection_param_update_t param_update = {0};
        param_update.conn_id = conn_id;
        param_update.interval_min = high_speed_interva_min;
        param_update.interval_max = high_speed_interva_max;
        param_update.max_latency = high_speed_latency;
        param_update.supervision_timeout = high_speed_timeout;
        errcode_t ret = sle_update_connect_param(&param_update);
        if (ret != ERRCODE_SUCC) {
            osal_printk("SLE连接参数更新失败，错误码：%u\n", ret);
        }

        // 更新PHY
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
            osal_printk("SLE PHY参数设置失败，错误码：%u\n", ret_phy);
        }

    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        osal_printk("SLE已断开，连接ID：%u\n", conn_id);
        sle::conn_id = 0; // 连接断开后重置连接ID

        // 清理数据缓冲区
        if (data_clear != nullptr) {
            data_clear();
        }

        // 每次重新广播前必须重新设置
        // 否则第2次连接时协议栈 MTU 能力未确认，可能影响数据传输
        set_mtu();

        // 必须完整重新初始化广播，不能只调start（原C代码也这样做）
        advertising_init();
        advertising_start();
    }
}

void sle::ssap_mtu_callback(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SUCC) {
        osal_printk("SLE MTU协商失败，错误码：%u\n", status);
        return;
    }
    true_mtu = param->mtu_size;
}

void sle::get_data_callback(uint8_t server_id, uint16_t conn_id, ssaps_req_write_cb_t *req_param, errcode_t status)
{
    unused(server_id);
    if (status != ERRCODE_SUCC) {
        osal_printk("SLE获取数据失败，错误码：%u\n", status);
        return;
    }
    if (data_process == nullptr) {
        osal_printk("SLE数据缓冲区未初始化\n");
        return;
    }
    data_process(req_param->value, req_param->length);
}
