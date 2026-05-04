#include "provisioner.hpp"

namespace sed_ws63 {

errcode_t softap_provisioner::run(sta &sta_obj, softapconfig config)
{
    // 关闭sta的连接
    sta_obj.sta_disconnect();

    // 启动SoftAP
    softap ap(config);
    errcode_t ret = ap.init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("配网softap启动失败，错误码: %u\n", ret);
        return 0x01; // 启动SoftAP失败，返回错误码
    }

    // udp阻塞接收凭据
    stacredential cred = {};
    char udp_bind_ip[16] = {0};
    sprintf(udp_bind_ip, "%u.%u.%u.%u", config.ip[0], config.ip[1], config.ip[2], config.ip[3]);
    // SED : 这里的端口号写死
    uint16_t udp_bind_port = 20261;
    ret = receive_credentials(&cred, udp_bind_ip, udp_bind_port);
    if (ret != ERRCODE_SUCC) {
        osal_printk("接收配网凭据失败，错误码: %u\n", ret);
        ap.deinit();
        return 0x02; // 接收凭据失败，返回错误码
    }

    // 关闭SoftAP
    ap.deinit();

    // 等待稳定
    osal_msleep(100);

    // sta连接正式网络
    ret = sta_obj.sta_connect(cred);
    if (ret != ERRCODE_SUCC) {
        osal_printk("连接配网凭据失败，错误码: %u\n", ret);
        return 0x03; // 连接失败，返回错误码
    }

    osal_printk("配网成功，已连接到SSID: %s\n", cred.ssid);
    return ERRCODE_SUCC;
}

errcode_t softap_provisioner::receive_credentials(stacredential *out, const char *udp_bind_ip, uint16_t udp_bind_port)
{
    udp udp_server;

    // 绑定UDP端口
    errcode_t ret = udp_server.bind_udp(udp_bind_port, udp_bind_ip);
    if (ret != ERRCODE_SUCC) {
        osal_printk("UDP绑定失败，错误码: %u\n", ret);
        return 0x04; // 绑定失败，返回错误码
    }

    char buf[udp_buffer_size_] = {0};
    int32_t len = udp_server.receive_udp((uint8_t *)buf, sizeof(buf));
    if (len <= 0) {
        osal_printk("接收UDP数据失败，错误码: %d\n", len);
        return 0x05; // 接收失败，返回错误码
    }

    cJSON *json = cJSON_Parse(buf);
    if (json == nullptr) {
        osal_printk("解析JSON失败，数据: %s\n", buf);
        return 0x06; // 接收失败，返回错误码
    }

    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *password_item = cJSON_GetObjectItem(json, "password");

    if (ssid_item == nullptr || password_item == nullptr || ssid_item->valuestring == nullptr ||
        password_item->valuestring == nullptr) {
        cJSON_Delete(json);
        osal_printk("JSON格式错误，缺少ssid或password字段\n");
        return 0x07; // JSON解析失败，返回错误码
    }

    // 检查长度
    size_t ssid_len = strlen(ssid_item->valuestring);
    size_t pwd_len = strlen(password_item->valuestring);
    if (ssid_len == 0 || ssid_len >= sizeof(out->ssid) || pwd_len == 0 || pwd_len >= sizeof(out->password)) {
        cJSON_Delete(json);
        osal_printk("SSID或密码长度不合法，SSID长度: %zu, 密码长度: %zu\n", ssid_len, pwd_len);
        return 0x08; // SSID或密码长度不合法，返回错误码
    }

    // 复制SSID和密码
    wifi_tool::copy_str(out->ssid, sizeof(out->ssid), ssid_item->valuestring);
    wifi_tool::copy_str(out->password, sizeof(out->password), password_item->valuestring);

    cJSON_Delete(json);

    return ERRCODE_SUCC;
}

} // namespace sed_ws63