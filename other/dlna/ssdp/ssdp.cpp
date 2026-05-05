#include "ssdp.hpp"

namespace sed_ws63 {
namespace {
void log_ssdp_packet(const sockaddr_in &peer_addr, int payload_len, const char *payload)
{
    if (payload == nullptr) {
        return;
    }

    uint32_t peer_ip = lwip_ntohl(peer_addr.sin_addr.s_addr);
    const char *line_end = strstr(payload, "\r\n");
    int line_len = (line_end != nullptr) ? static_cast<int>(line_end - payload) : payload_len;
    if (line_len < 0) {
        line_len = 0;
    }
    if (line_len > 120) {
        line_len = 120;
    }

    // SED_LOG: 串口调试打印，用于确认是否收到了SSDP报文，调试完成后应删除。
    osal_printk("dlna ssdp packet: from=%u.%u.%u.%u:%u len=%d first=%.*s\n", (peer_ip >> 24) & 0xFF,
                (peer_ip >> 16) & 0xFF, (peer_ip >> 8) & 0xFF, peer_ip & 0xFF, lwip_ntohs(peer_addr.sin_port),
                payload_len, line_len, payload);
}
} // namespace

ssdp::~ssdp()
{
    ssdp_close();
}

errcode_t ssdp::open_socket()
{
    ssdp_close(); // 先关闭之前的套接字，确保资源被正确释放

    // 创建UDP套接字
    sfd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) {
        return 0x01; // 创建套接字失败
    }

    int32_t reuse = 1;
    errcode_t ret = lwip_setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (ret < 0) {
        ssdp_close();
        return 0x02; // 设置套接字选项失败
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = lwip_htons(ssdp_port);
    ret = lwip_bind(sfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (ret != 0) {
        ssdp_close();
        return 0x03; // 绑定套接字失败
    }

    // 加入组播组
    ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = inet_addr(ssdp_multicast_addr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    ret = lwip_setsockopt(sfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    if (ret < 0) {
        ssdp_close();
        return 0x04; // 加入组播组失败
    }

    // SED_LOG: 串口调试打印，用于确认SSDP监听套接字已成功建立，调试完成后应删除。
    osal_printk("dlna ssdp socket ready: fd=%d port=%u mcast=%s\n", sfd, ssdp_port, ssdp_multicast_addr);

    return ERRCODE_SUCC; // 成功
}

errcode_t ssdp::process_once(const char *local_ip, uint16_t http_port, const char *udn)
{
    // 输入检查
    if (sfd < 0 || local_ip == nullptr || udn == nullptr) {
        return 0x05; // 参数错误
    }

    // 每次只处理一个请求，保持renderer主循环简单可控
    char buffer[1024] = {0};
    sockaddr_in peer_addr = {};
    socklen_t addr_len = sizeof(peer_addr);
    errcode_t ret =
        lwip_recvfrom(sfd, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr *>(&peer_addr), &addr_len);
    if (ret < 0) {
        // SED_LOG: 串口调试打印，用于确认SSDP接收失败分支是否被命中，调试完成后应删除。
        osal_printk("dlna ssdp recv failed: ret=%d\n", ret);
        return 0x06; // 接收数据失败
    }
    buffer[ret] = '\0'; // 确保字符串以null结尾
    log_ssdp_packet(peer_addr, ret, buffer);

    if (wifi_tool::strstr_s(buffer, "M-SEARCH") == nullptr) {
        // SED_LOG: 串口调试打印，用于区分非M-SEARCH报文，调试完成后应删除。
        osal_printk("dlna ssdp ignored non-M-SEARCH\n");
        return 0x07; // 不是M-SEARCH请求，忽略
    }

    // 只处理M-SEARCH请求，且来源IP必须是本地IP，防止处理非本地的SSDP请求
    char st[128] = {0};
    char man[128] = {0};
    char host[64] = {0};
    const wifi_tool::span_text st_span = wifi_tool::trim_and_find_http_header_value(buffer, "ST");
    const wifi_tool::span_text man_span = wifi_tool::trim_and_find_http_header_value(buffer, "MAN");
    const wifi_tool::span_text host_span = wifi_tool::trim_and_find_http_header_value(buffer, "HOST");
    if (st_span.ptr == nullptr || man_span.ptr == nullptr) {
        // SED_LOG: 串口调试打印，用于确认SSDP请求是否缺少旧版要求的关键头，调试完成后应删除。
        osal_printk("dlna ssdp missing header: st=%d man=%d host=%d\n", st_span.ptr != nullptr, man_span.ptr != nullptr,
                    host_span.ptr != nullptr);
        return 0x08; // 请求缺少必要的ST或MAN头部
    }
    ret = wifi_tool::copy_str(st, sizeof(st), st_span.ptr, st_span.len);
    if (ret != ERRCODE_SUCC) {
        return 0x09; // 复制ST头部值失败
    }
    ret = wifi_tool::copy_str(man, sizeof(man), man_span.ptr, man_span.len);
    if (ret != ERRCODE_SUCC) {
        return 0x0A; // 复制MAN头部值失败
    }
    if (host_span.ptr != nullptr) {
        ret = wifi_tool::copy_str(host, sizeof(host), host_span.ptr, host_span.len);
        if (ret != ERRCODE_SUCC) {
            return 0x0B; // 复制HOST头部值失败
        }
    }
    wifi_tool::trim(st);
    wifi_tool::trim(man);
    if (host[0] != '\0') {
        wifi_tool::trim(host);
    }

    if (wifi_tool::is_strstr_ignore_case(man, "ssdp:discover") == false) {
        // SED_LOG: 串口调试打印，用于确认MAN过滤原因，调试完成后应删除。
        osal_printk("dlna ssdp ignored: invalid MAN=%s\n", man);
        return 0x0C; // MAN头部值不是"ssdp:discover"，忽略
    }
    if (host[0] != '\0' && !wifi_tool::strcmp_ignore_case(host, "239.255.255.250:1900")) {
        // SED_LOG: 串口调试打印，用于确认HOST过滤原因，调试完成后应删除。
        osal_printk("dlna ssdp ignored: invalid HOST=%s\n", host);
        return 0x0D; // HOST头部值不正确，忽略
    }

    const bool is_ssdp_all = wifi_tool::strcmp_ignore_case(st, "ssdp:all");
    const bool is_root = wifi_tool::strcmp_ignore_case(st, "upnp:rootdevice");
    const bool is_uuid = wifi_tool::strcmp_ignore_case(st, udn);
    const bool is_renderer = wifi_tool::strcmp_ignore_case(st, "urn:schemas-upnp-org:device:MediaRenderer:1");
    const bool is_avt = wifi_tool::strcmp_ignore_case(st, "urn:schemas-upnp-org:service:AVTransport:1");
    const bool is_rcs = wifi_tool::strcmp_ignore_case(st, "urn:schemas-upnp-org:service:RenderingControl:1");
    const bool is_cm = wifi_tool::strcmp_ignore_case(st, "urn:schemas-upnp-org:service:ConnectionManager:1");
    const bool is_qplay = wifi_tool::strstr_s(st, "QPlay") != nullptr;

    if (!(is_ssdp_all || is_root || is_uuid || is_renderer || is_avt || is_rcs || is_cm || is_qplay)) {
        // SED_LOG: 串口调试打印，用于确认ST过滤原因，调试完成后应删除。
        osal_printk("dlna ssdp ignored: unsupported ST=%s\n", st);
        return ERRCODE_SUCC; // ST头部值不匹配，虽然是M-SEARCH请求但不是针对本设备的，忽略
    }

    // SED_LOG: 串口调试打印，用于确认识别到M-SEARCH后的ST，调试完成后应删除。
    osal_printk("ssdp收到M-SEARCH: ST=%s\n", st);

    if (is_ssdp_all) {
        // ssdp:all 需要逐条回复多个目标，不能偷懒合并成一条
        send_reply(peer_addr, addr_len, local_ip, http_port, udn, "UPnP:rootdevice", "UPnP:rootdevice");
        osal_msleep(20);
        send_reply(peer_addr, addr_len, local_ip, http_port, udn, udn);
        osal_msleep(20);
        send_reply(peer_addr, addr_len, local_ip, http_port, udn, "urn:schemas-upnp-org:device:MediaRenderer:1",
                   "urn:schemas-upnp-org:device:MediaRenderer:1");
        osal_msleep(20);
        send_reply(peer_addr, addr_len, local_ip, http_port, udn, "urn:schemas-upnp-org:service:AVTransport:1",
                   "urn:schemas-upnp-org:service:AVTransport:1");
        osal_msleep(20);
        send_reply(peer_addr, addr_len, local_ip, http_port, udn, "urn:schemas-upnp-org:service:RenderingControl:1",
                   "urn:schemas-upnp-org:service:RenderingControl:1");
        osal_msleep(20);
        send_reply(peer_addr, addr_len, local_ip, http_port, udn, "urn:schemas-upnp-org:service:ConnectionManager:1",
                   "urn:schemas-upnp-org:service:ConnectionManager:1");
        return ERRCODE_SUCC;
    }

    if (is_root) {
        return send_reply(peer_addr, addr_len, local_ip, http_port, udn, st, st);
    }
    if (is_uuid) {
        return send_reply(peer_addr, addr_len, local_ip, http_port, udn, st);
    }

    return send_reply(peer_addr, addr_len, local_ip, http_port, udn, st, st);
}

errcode_t ssdp::send_reply(const sockaddr_in &peer_addr,
                           const socklen_t peer_len,
                           const char *local_ip,
                           uint16_t http_port,
                           const char *udn,
                           const char *st,
                           const char *usn_suffix)
{
    char usn[256] = {0};

    // 设备uuid响应和带服务后缀的usn格式不同，这里统一拼装
    if (usn_suffix != nullptr && usn_suffix[0] != '\0' && !wifi_tool::strcmp_ignore_case(st, udn)) {
        // 非 uuid 请求且提供了后缀时，USN 需要带上 "::suffix"。
        snprintf(usn, sizeof(usn), "%s::%s", udn, usn_suffix);
    } else {
        // uuid请求或者没有usn_suffix，usn就是设备uuid
        snprintf(usn, sizeof(usn), "%s", udn);
    }

    char response[896] = {0};
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "CACHE-CONTROL: max-age=%d\r\n"
             "EXT:\r\n"
             "LOCATION: http://%s:%u/description.xml\r\n"
             "SERVER: Linux/5.10 UPnP/1.1 WS63/1.0\r\n"
             "ST: %s\r\n"
             "USN: %s\r\n"
             "BOOTID.UPNP.ORG: %d\r\n"
             "CONFIGID.UPNP.ORG: %d\r\n\r\n",
             ssdp_max_age, local_ip, http_port, st, usn, ssdp_boot_id, ssdp_config_id);

    errcode_t ret =
        lwip_sendto(sfd, response, strlen(response), 0, reinterpret_cast<const sockaddr *>(&peer_addr), peer_len);
    if (ret < 0) {
        return 0x0E; // 发送响应失败
    }

    // SED_LOG: 串口调试打印，用于确认SSDP响应已发出，调试完成后应删除。
    osal_printk("ssdp已响应M-SEARCH (ST=%s, USN=%s)\n", st, usn);

    return ERRCODE_SUCC; // 成功
}

void ssdp::ssdp_close()
{
    if (sfd >= 0) {
        lwip_close(sfd);
        sfd = -1;
    }
}

} // namespace sed_ws63