#include "sle.hpp"

std::array<sle::connection_device, sle::max_connection_num> sle::connection_devices = {0};
sle_addr_t sle::s_pending_addr = {0};

sle::sle()
{
    osal_msleep(1000);
    // 启用回调函数
    static sle_announce_seek_callbacks_t a = {0};
    a.sle_enable_cb = sle_enable_callback;
    a.seek_result_cb = seek_finded_callback;
    a.seek_disable_cb = seek_disable_callback;

    static sle_connection_callbacks_t b = {0};
    b.connect_state_changed_cb = connect_changed_callback;
    b.set_phy_cb = after_phy_set_callback;

    static ssapc_callbacks_t c = {0};
    c.find_structure_cb = find_service_callback;
    c.ssapc_find_property_cbk = find_property_callback;
    c.exchange_info_cb = ssap_mtu_callback;

    sle_announce_seek_register_callbacks(&a);
    sle_connection_register_callbacks(&b);
    ssapc_register_callbacks(&c);

    errcode_t ret = enable_sle();
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE使能失败，错误码：%u\n", ret);
    }
}

void sle::sle_enable_callback(errcode_t status)
{
    if (status != ERRCODE_SUCC) {
        osal_printk("SLE使能回调失败，错误码：%u\n", status);
        return;
    }

    uint16_t nv_value_len = 0;
    uint8_t nv_value = 0;

    uapi_nv_read(nv_key, sizeof(uint8_t), &nv_value_len, &nv_value);
    if (nv_value != nv_tx) {
        nv_value = nv_tx;
        errcode_t nv_ret = uapi_nv_write(nv_key, &nv_value, sizeof(nv_value));
        if (nv_ret != ERRCODE_SUCC) {
            osal_printk("NV写入失败，错误码：%u\n", nv_ret);
        }
    }

    set_local_address();

    sle_default_connect_param_t connect_param = {0};
    connect_param.enable_filter_policy = enable_filter_policy;
    connect_param.initiate_phys = enable_phys;
    connect_param.gt_negotiate = gt_negotiate;
    connect_param.scan_interval = scan_interval;
    connect_param.scan_window = scan_window;
    connect_param.min_interval = scan_interval_min;
    connect_param.max_interval = scan_interval_max;
    connect_param.timeout = scan_timeout;
    errcode_t ret = sle_default_connection_param_set(&connect_param);
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE默认连接参数设置失败，错误码：%u\n", ret);
        return;
    }

    start_seek();
}

void sle::write_send(int index, uint8_t *data, uint16_t len_b)
{
    if (index < 0 || index >= max_connection_num) {
        osal_printk("发送失败1: index无效 index=%d\n", index);
        return;
    }
    if (!connection_devices[index].is_active) {
        osal_printk("发送失败1: 设备未激活 index=%d\n", index);
        return;
    }
    if (connection_devices[index].target_handle == 0) {
        osal_printk("发送失败1: target_handle为0 index=%d, client_id=%u, conn_id=%u\n", index,
                    connection_devices[index].client_id, connection_devices[index].conn_id);
        return;
    }

    ssapc_write_param_t write_param = {0};
    write_param.handle = connection_devices[index].target_handle;
    write_param.type = find_write_type;
    write_param.data_len = len_b;
    write_param.data = data;
    errcode_t ret =
        ssapc_write_cmd(connection_devices[index].client_id, connection_devices[index].conn_id, &write_param);

    if (ret != ERRCODE_SUCC) {
        static int fail_cnt = 0;
        if (fail_cnt++ % 100 == 0) {
            osal_printk("发送失败2: ret=0x%x, mtu=%u, len=%u\n", ret, connection_devices[index].mtu, len_b);
        }
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

void sle::start_seek()
{
    sle_seek_param_t scan_param = {0};
    scan_param.own_addr_type = own_addr_type;
    scan_param.filter_duplicates = filter_duplicates;
    scan_param.seek_filter_policy = seek_filter_policy;
    scan_param.seek_phys = seek_phys;
    scan_param.seek_type[0] = seek_type;
    scan_param.seek_interval[0] = seek_interval;
    scan_param.seek_window[0] = seek_window;
    errcode_t ret = sle_set_seek_param(&scan_param);
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE扫描参数设置失败，错误码：%u\n", ret);
        return;
    }
    ret = sle_start_seek();
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE启动扫描失败，错误码：%u\n", ret);
    }
}

void sle::seek_finded_callback(sle_seek_result_info_t *seek_result_data)
{
    // 由于星闪官方没有实现白名单，这里手动处理连接到的设备的地址匹配
    if (seek_result_data->addr.addr[0] == peer_address[0] && seek_result_data->addr.addr[1] == peer_address[1] &&
        seek_result_data->addr.addr[2] == peer_address[2] && seek_result_data->addr.addr[3] == peer_address[3] &&
        seek_result_data->addr.addr[4] == peer_address[4] && seek_result_data->addr.addr[5] == peer_address[5]) {

        int index = find_free_connection_device_conned();
        if (index == -1) {
            return; // 所有槽位已被占用或正在连接中，忽略此次回调
        }
        connection_devices[index].is_pending = true;
        s_pending_addr = seek_result_data->addr; // 保存地址，在 seek_disable_cb 中使用
        sle_stop_seek();
    }
}

int sle::find_connectioned_device_connid(uint16_t conn_id)
{
    for (int i = 0; i < max_connection_num; ++i) {
        // 查找连接设备列表中是否有对应conn_id的设备
        // 并且需要该设备目前是活跃的连接状态
        if (connection_devices[i].conn_id == conn_id && connection_devices[i].is_active) {
            return i;
        }
    }
    return -1; // 未找到
}

int sle::find_pending_butnot_active_device()
{
    for (int i = 0; i < max_connection_num; ++i) {
        // 查找连接设备列表中是否有正在连接但尚未活跃的设备
        if (connection_devices[i].is_pending && !connection_devices[i].is_active) {
            return i;
        }
    }
    return -1; // 未找到
}

void sle::seek_disable_callback(errcode_t status)
{
    if (status != ERRCODE_SUCC) {
        // 扫描停止失败，释放 pending 槽位允许重试
        for (int i = 0; i < max_connection_num; ++i) {
            if (connection_devices[i].is_pending) {
                connection_devices[i].is_pending = false;
            }
        }
        return;
    }
    errcode_t ret = sle_connect_remote_device(&s_pending_addr);
    if (ret != ERRCODE_SUCC) {
        osal_printk("SLE连接设备失败，错误码：%u\n", ret);
        for (int i = 0; i < max_connection_num; ++i) {
            if (connection_devices[i].is_pending) {
                connection_devices[i].is_pending = false;
            }
        }
    } else {
    }
}

int sle::find_free_connection_device_conned()
{
    for (int i = 0; i < max_connection_num; ++i) {
        // 槽位空闲 = 既没有活跃连接，也没有正在发起的连接
        if (!connection_devices[i].is_active && !connection_devices[i].is_pending) {
            return i;
        }
    }
    return -1; // 未找到空闲槽位
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
        // 连接成功：找到之前标记为 pending 的槽位，转为 active
        int index = -1;
        for (int i = 0; i < max_connection_num; ++i) {
            if (connection_devices[i].is_pending) {
                index = i;
                break;
            }
        }
        if (index != -1) {
            // 找到空闲槽位，保存连接信息
            connection_devices[index].conn_id = conn_id;
            connection_devices[index].peer_addr = *addr;
            connection_devices[index].is_active = true;
            connection_devices[index].is_pending = false;
            // peer_addr 已在 seek_finded_callback 中保存
        }
        // 调整对应连接上的设备的连接参数，开放高速连接
        sle_connection_param_update_t param_update = {0};
        param_update.conn_id = conn_id;
        param_update.interval_min = high_speed_interva_min;
        param_update.interval_max = high_speed_interva_max;
        param_update.max_latency = high_speed_latency;
        param_update.supervision_timeout = high_speed_timeout;
        errcode_t ret1 = sle_update_connect_param(&param_update);
        if (ret1 != ERRCODE_SUCC) {
            osal_printk("SLE更新连接参数失败，错误码：%u\n", ret1);
        }

        // 调整PHY，开放4M高速通道
        sle_set_phy_t phy_param = {0};
        phy_param.tx_format = format;
        phy_param.rx_format = format;
        phy_param.tx_phy = phy;
        phy_param.rx_phy = phy;
        phy_param.tx_pilot_density = pilot_density;
        phy_param.rx_pilot_density = pilot_density;
        phy_param.g_feedback = feedback;
        phy_param.t_feedback = feedback;
        errcode_t ret2 = sle_set_phy_param(conn_id, &phy_param);
        if (ret2 != ERRCODE_SUCC) {
            osal_printk("SLE设置PHY参数失败，错误码：%u\n", ret2);
        }

        if (find_free_connection_device_conned() != -1) {
            // 没有满的话，继续扫描，寻找其他设备（如果有的话）
            start_seek();
            // 不 return，对当前已连接设备启动服务发现
        }

    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        int index = find_connectioned_device_connid(conn_id);
        if (index != -1) {
            // 找到对应连接设备，清理资源(恢复默认值)
            // 先释放 SSAP 客户端注册槽位，否则每次重连泄漏一个槽位
            // WS63 协议栈槽位有限，2~3次后 ssapc_register_client 失败导致后续发送异常
            if (connection_devices[index].client_id != 0) {
                ssapc_unregister_client(connection_devices[index].client_id);
            }
            connection_devices[index].conn_id = -1;
            connection_devices[index].client_id = 0;
            connection_devices[index].target_handle = 0;
            connection_devices[index].device_type = 0;
            connection_devices[index].peer_addr = {0};
            connection_devices[index].is_active = false;
            connection_devices[index].is_pending = false;
            connection_devices[index].mtu = 0;
        } else {
            // 寻找正在连接但不活跃的设备槽位，即连接被拒绝的设备槽位，清理资源
            int pending_index = find_pending_butnot_active_device();
            if (pending_index != -1) {
                connection_devices[pending_index].is_pending = false;
            }
        }

        // 连接断开，重新扫描
        start_seek();
    }
}

void sle::find_service(int index)
{
    ssapc_find_structure_param_t ssapc_param = {0};
    ssapc_param.type = find_service_type;
    ssapc_param.start_hdl = start_hdl;
    ssapc_param.end_hdl = end_hdl;
    ssapc_param.uuid.len = 2;
    ssapc_param.uuid.uuid[0] = uuid_service_audio_1;
    ssapc_param.uuid.uuid[1] = uuid_service_audio_2;

    errcode_t ret =
        ssapc_find_structure(connection_devices[index].client_id, connection_devices[index].conn_id, &ssapc_param);
    if (ret != ERRCODE_SUCC) {
        osal_printk("SSAP查找服务失败，错误码：%u\n", ret);
    }
}

void sle::find_service_callback(uint8_t client_id, uint16_t conn_id, ssapc_find_service_result_t *svc, errcode_t status)
{
    (void)client_id;
    if (status != ERRCODE_SUCC) {
        osal_printk("设置服务失败，错误码：%u\n", status);
        return;
    }
    if (svc == NULL) {
        osal_printk("设置服务失败，svc为空指针\n");
        return;
    }
    if (svc->uuid.len != 2 && svc->uuid.len != 16) {
        osal_printk("设置服务失败，uuid.len=%u (unexpected)\n", svc->uuid.len);
        return;
    }

    // uuid[0..1] 始终存放16-bit UUID（无论是2字节还是128字节展开格式）
    uint16_t uuid = (svc->uuid.uuid[1] << 8) | svc->uuid.uuid[0];
    int index = find_connectioned_device_connid(conn_id);
    if (index != -1) {
        if (uuid == uuid_service) {
        }

        ssapc_find_structure_param_t ssapc_param = {0};
        ssapc_param.type = find_property_type;
        ssapc_param.start_hdl = svc->start_hdl;
        ssapc_param.end_hdl = svc->end_hdl;

        errcode_t ret = ssapc_find_structure(connection_devices[index].client_id, conn_id, &ssapc_param);
        if (ret != ERRCODE_SUCC) {
            osal_printk("SSAP查找属性失败，错误码：%u\n", ret);
        }
    } else {
        osal_printk("SSAP查找服务回调参数错误，错误码：%u\n", status);
    }
}

void sle::find_property_callback(uint8_t client_id,
                                 uint16_t conn_id,
                                 ssapc_find_property_result_t *property,
                                 errcode_t status)
{
    (void)client_id;
    if (status != ERRCODE_SUCC || property == NULL) {
        return;
    }
    if (property->uuid.len != 2 && property->uuid.len != 16) {
        return;
    }
    int index = find_connectioned_device_connid(conn_id);
    if (index == -1) {
        osal_printk("SSAP查找属性回调参数错误，错误码：%u\n", status);
        return;
    }

    uint16_t uuid = 0;
    if (property->uuid.len == 2) {
        uuid = (property->uuid.uuid[1] << 8) | property->uuid.uuid[0];
    } else if (property->uuid.len == 16) {
        // 尝试从 128-bit UUID 中提取 16-bit UUID
        // 检查 0-1, 12-13, 14-15
        uint16_t u0 = (property->uuid.uuid[1] << 8) | property->uuid.uuid[0];
        uint16_t u12 = (property->uuid.uuid[13] << 8) | property->uuid.uuid[12];
        uint16_t u14 = (property->uuid.uuid[15] << 8) | property->uuid.uuid[14];

        if (u0 == uuid_property)
            uuid = u0;
        else if (u12 == uuid_property)
            uuid = u12;
        else if (u14 == uuid_property)
            uuid = u14;
        else
            uuid = u0;
    }

    if (uuid == uuid_property) {
        // 找到对应属性，保存目标特征句柄，准备发送数据
        osal_msleep(100);
        connection_devices[index].target_handle = property->handle;
    } else {
        osal_printk("SSAP查找属性回调参数错误，错误码：%u\n", status);
    }
}

void sle::ssap_connect(uint16_t conn_id)
{
    // 注册SSAP客户端，准备发送数据
    int index = find_connectioned_device_connid(conn_id);
    if (index == -1) {
        osal_printk("SSAP连接失败，未找到conn_id对应设备：%u\n", conn_id);
        return;
    }

    sle_uuid_t service_uuid = {0};
    service_uuid.len = 2;
    service_uuid.uuid[0] = uuid_user_1;
    service_uuid.uuid[1] = uuid_user_2;

    // 防御性清理：如果因某种原因未经断开流程就再次进入，先释放旧注册
    if (connection_devices[index].client_id != 0) {
        ssapc_unregister_client(connection_devices[index].client_id);
        connection_devices[index].client_id = 0;
    }

    errcode_t ret1 = ssapc_register_client(&service_uuid, &connection_devices[index].client_id);
    if (ret1 != ERRCODE_SUCC) {
        osal_printk("SSAP注册失败，错误码：%u\n", ret1);
    }

    // 开启mtu协商
    ssap_exchange_info_t exchange_info = {0};
    exchange_info.mtu_size = mtu_max;
    exchange_info.version = 1; // 版本预留字段
    errcode_t ret2 = ssapc_exchange_info_req(connection_devices[index].client_id, conn_id, &exchange_info);
    if (ret2 != ERRCODE_SUCC) {
        osal_printk("SSAP协商MTU失败，错误码：%u\n", ret2);
    }
}

void sle::ssap_mtu_callback(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SUCC) {
        return;
    }

    int index = find_connectioned_device_connid(conn_id);
    if (index != -1) {
        connection_devices[index].mtu = param->mtu_size;
        // 使用协商后的真实 MTU 设置链路层 PDU，而不是本端硬编码的 mtu_max
        errcode_t ret1 = sle_set_data_len(conn_id, param->mtu_size);
        if (ret1 != ERRCODE_SUCC) {
            osal_printk("MTU设置数据长度失败，错误码：%u\n", ret1);
        }
        find_service(index);
    }
}

void sle::after_phy_set_callback(uint16_t conn_id, errcode_t status, const sle_set_phy_t *param)
{
    unused(param);
    if (status == ERRCODE_SUCC) {
        errcode_t ret = sle_set_mcs(conn_id, mcs);
        if (ret != ERRCODE_SUCC) {
            osal_printk("SLE设置MCS失败，错误码：%u\n", ret);
        }

        // PHY设置成功后，才认为连接完全准备好，可以发送数据
        ssap_connect(conn_id);
    } else {
        osal_printk("SLE设置PHY失败，错误码：%u\n", status);
        // 清理连接状态，触发断开流程，重新扫描连接
        int index = find_connectioned_device_connid(conn_id);
        if (index != -1) {
            // 找到对应连接设备，清理资源(恢复默认值)
            // 先释放 SSAP 客户端注册槽位，否则每次重连泄漏一个槽位
            // WS63 协议栈槽位有限，2~3次后 ssapc_register_client 失败导致后续发送异常
            if (connection_devices[index].client_id != 0) {
                ssapc_unregister_client(connection_devices[index].client_id);
            }
            connection_devices[index].conn_id = -1;
            connection_devices[index].client_id = 0;
            connection_devices[index].target_handle = 0;
            connection_devices[index].device_type = 0;
            connection_devices[index].peer_addr = {0};
            connection_devices[index].is_active = false;
            connection_devices[index].is_pending = false;
            connection_devices[index].mtu = 0;
        }
    }
}