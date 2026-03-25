# 基于海思 WS63 的 C++ DLNA (DMR) 与音频解码实现指南 (进阶版)

## 一、 核心问题解答：能否跳过解码，直接接收 PCM？

**结论：无法通过正规音乐软件（如QQ音乐、网易云）直接推送 PCM。如果你希望被主流软件识别并投屏，必须在 WS63 上实现这两种格式之一的解码（主要是 MP3 或 AAC）。**

*   **原理解析：** QQ音乐在点击 DLNA 投屏时，扮演的是 DMC（控制器）的角色，它发给音响（DMR）的是一个 **公网的 URL 链接**，一般是一首 `.mp3` 或 `.m4a` 文件。音乐 App 不会在手机端把音乐解压成 PCM 再以串流推给音响，因为那样对手机的带宽和电量消耗极大。
*   **如何解决 WS63 没有硬件解码的问题？**
    WS63 虽然是轻量级 MCU，但其算力（一般能到 160MHz+ 甚至更高）完全足以应对 **MP3 纯软件解码**。
    强烈推荐引入 **`minimp3`** 这个开源库（GitHub上搜 `minimp3`）。它只有一个 `.h` 头文件，没有外部依赖，ROM 占用只有约 `40KB`，RAM 占用约 `20KB`，非常适合 WS63！你只需要用 socket 下载流，输入给它，就能立刻得到可写入 I2S 驱动的 PCM 数组。

---

## 二、 C++ 面向对象架构设计：`Wifi` 类与 `DlnaDmr` 类

你的思路非常优秀。将通信底层（WiFi/UDP）与应用层（DLNA 设备端）拆分到不同类中，可以极大提升代码的可维护性。

此时系统应该有这样的架构：
1.  **`wifi` 类**：负责底层的 SoftAP 配网、STA 连接路由器、以及建立通过 UDP/JSON 接收账号密码的任务。
2.  **`DlnaDmr` 类**：当 `wifi` 类成功连接到家中路由器后被实例化与启动。它包含以下核心功能：
    *   **DLNA 通信主任务 (单线程多路复用)**：这是最核心的进阶优化！为了极大地节省 WS63 的 Task 内存（RAM），我们不要为 SSDP 和 HTTP 单独开两个线程。我们在一个 Task 中使用 LwIP 的 `select()` 机制，同时无阻塞地监听 SSDP（UDP 1900端口）和 HTTP（TCP 49152端口）。
    *   **Audio Player 任务**：获取到 URL 后，发起 HTTP GET 下载流媒体，喂给 `minimp3` 并通过 I2S 输出给喇叭。这个必须是独立线程，以防解码阻塞网络监听。

### 架构时序图
```text
[系统上电] -> 初始化 wifi 类 
           -> wifi.restart_get_wifi() (配网) 
           -> 成功连接到家庭路由器
           -> 实例化 DlnaDmr dlna_worker;
           -> dlna_worker.start(); 
                  -> 启动 dlna_main_task (使用 select 同时监听 UDP & TCP)
                  -> 进入低功耗待命状态...
           [QQ音乐搜索设备] -> dlna_main_task 捕获 UDP -> 响应 SSDP 宣告自己
           [QQ音乐推送歌曲] -> dlna_main_task 捕获 TCP -> 解析 SOAP
                  -> 唤醒 Audio 线程接管 -> MP3 软解 -> I2S 发声
```

---

## 三、 `DlnaDmr` 类的代码结构示例

我们可以在 `dlan` 文件夹下建立 `dlna_dmr.hpp` 和 `dlna_dmr.cpp`。

### 1. `dlna_dmr.hpp` 框架
```cpp
#ifndef __DLNA_DMR_HPP__
#define __DLNA_DMR_HPP__

extern "C" {
#include "lwip/sockets.h"
#include "soc_osal.h"
// 引入你的 mp3 定义或 I2S 头文件
}
#include <string>

class DlnaDmr {
public:
    DlnaDmr();
    ~DlnaDmr();

    // 启动 DLNA 的主入口
    void start();

private:
    // 常量定义
    static constexpr uint16_t SSDP_PORT = 1900;
    static constexpr uint16_t HTTP_PORT = 49152;
    static constexpr const char* MCAST_IP = "239.255.255.250";

    // 线程安全参数
    int ssdp_sock;
    int http_sock;
    bool is_running;

    // 独立运行的 Task (使用 select 实现单线程监听两个网络，不仅安全且极大节省内存)
    static void dlna_main_task(void* arg);    // 统一处理 SSDP 与 HTTP
    static void audio_player_task(void* arg); // 专门处理流媒体音频下载与解码

    // Socket 初始化辅助函数
    int setup_ssdp_socket();
    int setup_http_socket();

    // 内部动作函数
    void handle_ssdp_msearch(int sock);
    void handle_http_request(int server_sock);
    
    // SOAP 解析辅助函数
    std::string extract_xml_tag(const std::string& xml, const std::string& tag);
};

#endif // __DLNA_DMR_HPP__
```

### 2. `dlna_dmr.cpp` 中的关键实现

**A. 启动线程 (单线程多路复用)**
```cpp
#include "dlna_dmr.hpp"

void DlnaDmr::start() {
    is_running = true;
    // 重大优化：把原本的两个独立网络监听 Task 合并为一个基于 select 的单一任务
    // 极大地节省了 WS63 宝贵的 Task 栈空间资源 (几 KB RAM)！
    osal_task_create("dlna_main", dlna_main_task, this, 8192, osal_task_get_current_prio());
}
```

**B. 基于 `select()` 的统一监听实现 (核心精髓)**

这段代码展现了如何用一个死循环同时等待 SSDP 的搜索以及 HTTP 控制指令，利用系统底层的 `select` 挂起自身，既不会错过消息，又丝毫不浪费 CPU。

```cpp
void DlnaDmr::dlna_main_task(void* arg) {
    DlnaDmr* dmr = static_cast<DlnaDmr*>(arg);
    
    // 1. 各自建立并绑定好两个 Socket
    dmr->ssdp_sock = dmr->setup_ssdp_socket(); // 申请 UDP，绑定1900端口，加入多播组
    dmr->http_sock = dmr->setup_http_socket(); // 申请 TCP，绑定49152端口，并开启 listen()
    
    if (dmr->ssdp_sock < 0 || dmr->http_sock < 0) {
        osal_printk("DLNA Socket 创建失败!\n");
        return;
    }

    fd_set read_fds;
    // 取两者之间最大的文件描述符传给 select
    int max_fd = (dmr->ssdp_sock > dmr->http_sock) ? dmr->ssdp_sock : dmr->http_sock;

    while (dmr->is_running) {
        // 每次循环必须清空并重新设置监听集合
        FD_ZERO(&read_fds);
        FD_SET(dmr->ssdp_sock, &read_fds); // 把 UDP 监控加进去
        FD_SET(dmr->http_sock, &read_fds); // 把 TCP 监控加进去

        // 【关键卡点】代码走到这里会进入系统级休眠！
        // 直到有人发了多播搜索，或者有人尝试发起 TCP 请求连接，系统才会唤醒这行代码向下走
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, NULL);

        if (ret > 0) {
            // 情形 1：如果是局域网手机发来了找音响的 M-SEARCH 广播 (UDP 变活跃)
            if (FD_ISSET(dmr->ssdp_sock, &read_fds)) {
                dmr->handle_ssdp_msearch(dmr->ssdp_sock); 
            }
            
            // 情形 2：如果是刚才搜跑的手机发来了具体的播放/暂停指令 (TCP 变活跃有新连接)
            if (FD_ISSET(dmr->http_sock, &read_fds)) {
                dmr->handle_http_request(dmr->http_sock);
            }
        } else if (ret < 0) {
            osal_printk("Select 发生错误!\n");
            break; // 错误处理防死锁
        }
    }
}

// 单独抽离的方法1：专门应答 UDP 多播
void DlnaDmr::handle_ssdp_msearch(int sock) {
    char buffer[1024];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // 因为外层 select 保证了有数据，这里 recvfrom 会秒接，不会阻塞卡死
    int len = recvfrom(sock, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&client_addr, &addr_len);
    if (len <= 0) {
        return;
    }
    
    buffer[len] = '\0';
    
    // 验证是否为有效的SSDP M-SEARCH请求
    if (strstr(buffer, "M-SEARCH") == NULL) {
        return;
    }
    
    // 验证必要的SSDP头
    if (strstr(buffer, "MAN: \"ssdp:discover\"") == NULL) {
        return;
    }
    
    // 检查是否请求了该设备类型
    if (strstr(buffer, "urn:schemas-upnp-org:device:MediaRenderer:1") == NULL) {
        return;
    }
    
    // 构建SSDP响应（动态获取设备IP需要从全局或参数获取）
    char response[768];
    int written = snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\n"
                 "CACHE-CONTROL: max-age=1800\r\n"
                 "EXT:\r\n"
                 "LOCATION: http://192.168.x.x:49152/description.xml\r\n" // 需要替换为 WS63 获取的真实本网IP
                 "SERVER: HiSilicon-WS63_sound DLNA DMR/1.0 UPnP/1.1\r\n"
                 "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                 "USN: uuid:20260321-1612-2007-0423-a1b2c3d4e5f6::urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                 "\r\n");
    
    if (written > 0 && written < sizeof(response)) {
        sendto(sock, response, written, 0, (struct sockaddr*)&client_addr, addr_len);
        osal_printk("已响应SSDP M-SEARCH\n");
    }
}
```

**C. HTTP Server 解析控制指令 (无状态即时通讯)**

一定要牢记，DLNA/UPnP 中，设备发现（UDP）和设备控制（TCP）是完全拆分的。手机端发送 `setAVTransportURI` 指定完网络 URL 后，就会**马上断开TCP连接**，我们也是即接即断，不存在所谓长连接！

```cpp
// 单独抽离的方法2：专门接收并解析 TCP 控制和设备描述
void DlnaDmr::handle_http_request(int server_sock) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    // 刚才 select 已经探知到了新连接到来，这里 accept 不会卡死，瞬间拿掉
    int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
    if (client_sock < 0) return;

    char buffer[2048];
    int len = recv(client_sock, buffer, sizeof(buffer)-1, 0);
    if (len <= 0) {
        lwip_close(client_sock);  // 注：如果在 LwIP，别忘了是 lwip_close
        return;
    }
    buffer[len] = '\0';
    osal_printk("[HTTP请求]:\n%s\n", buffer);

    // 打印首行便于串口调试
    char* line_end = strstr(buffer, "\r\n");
    if (line_end != NULL) {
        *line_end = '\0';
    }
    osal_printk("http请求首行: %s\n", buffer);
    if (line_end != NULL) {
        *line_end = '\r';
    }
    
    // ===== 情形1：请求设备描述 XML =====
    if (strstr(buffer, "GET /description.xml") || strstr(buffer, "HEAD /description.xml") ||
        strstr(buffer, "GET / HTTP/1.1") || strstr(buffer, "GET / HTTP/1.0")) {
        osal_printk("http收到设备描述请求\n");

        std::array<char, 2048> xml_response;
        std::array<char, 256> header;

        snprintf(xml_response.data(), xml_response.size(),
                 "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                 "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">\r\n"
                 "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
                 "  <device>\r\n"
                 "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\r\n"
                 "    <friendlyName>%s</friendlyName>\r\n"
                 "    <manufacturer>%s</manufacturer>\r\n"
                 "    <manufacturerURL>http://www.hisilicon.com/</manufacturerURL>\r\n"
                 "    <modelDescription>%s</modelDescription>\r\n"
                 "    <modelName>WS63-DMR</modelName>\r\n"
                 "    <modelNumber>%.1f</modelNumber>\r\n"
                 "    <UDN>%s</UDN>\r\n"
                 "    <dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>\r\n"
                 "    <serviceList>\r\n"
                 "      <service>\r\n"
                 "        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>\r\n"
                 "        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>\r\n"
                 "        <SCPDURL>/AVTransport.xml</SCPDURL>\r\n"
                 "        <controlURL>/AVTransport/control</controlURL>\r\n"
                 "        <eventSubURL>/AVTransport/event</eventSubURL>\r\n"
                 "      </service>\r\n"
                 "      <service>\r\n"
                 "        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>\r\n"
                 "        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>\r\n"
                 "        <SCPDURL>/RenderingControl.xml</SCPDURL>\r\n"
                 "        <controlURL>/RenderingControl/control</controlURL>\r\n"
                 "        <eventSubURL>/RenderingControl/event</eventSubURL>\r\n"
                 "      </service>\r\n"
                 "      <service>\r\n"
                 "        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>\r\n"
                 "        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>\r\n"
                 "        <SCPDURL>/ConnectionManager.xml</SCPDURL>\r\n"
                 "        <controlURL>/ConnectionManager/control</controlURL>\r\n"
                 "        <eventSubURL>/ConnectionManager/event</eventSubURL>\r\n"
                 "      </service>\r\n"
                 "    </serviceList>\r\n"
                 "  </device>\r\n"
                 "</root>\r\n",
                 http_xml_name.data(),
                 http_xml_manufacturer.data(),
                 http_xml_model_description.data(),
                 http_xml_version,
                 ssdp_uuid.data());

        int response_length = strlen(xml_response.data());
        snprintf(header.data(), header.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "Server: OS/1.0 UPnP/1.1 product/1.0\r\n\r\n",
                 response_length);

        lwip_send(client_sock, header.data(), strlen(header.data()), 0);
        lwip_send(client_sock, (const uint8_t*)xml_response.data(), strlen(xml_response.data()), 0);
        osal_printk("已发送设备描述XML\n");
    }
    
    // ===== 情形2：处理控制类 SOAP 请求（与 XML 中 controlURL 对齐） =====
    // 这里不再用“正文里是否包含 Play/Pause”做粗糙判断，
    // 而是先看 POST 的目标路径，再看 SOAPACTION，避免误判与串动作。
    else if (strstr(buffer, "POST ") != NULL) {
        // 第一步：校验请求路径是否命中设备描述 XML 中声明过的 controlURL。
        // 只有命中这些路径，控制端才算“按你声明的接口”在访问。
        const bool is_avt_control = strstr(buffer, "POST /AVTransport/control") != NULL;
        const bool is_rc_control = strstr(buffer, "POST /RenderingControl/control") != NULL;
        const bool is_cm_control = strstr(buffer, "POST /ConnectionManager/control") != NULL;

        if (!(is_avt_control || is_rc_control || is_cm_control)) {
            // 路径不在设备声明的 controlURL 范围内，返回 404 更准确
            // 语义：资源不存在（路径错），而不是请求格式错。
            static const char k404[] = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
            lwip_send(client_sock, (const uint8_t*)k404, sizeof(k404) - 1, 0);
        } else {
            // 按 SOAPACTION 做动作分发，避免仅靠正文关键字误判
            // 典型头格式：SOAPACTION: "urn:schemas-upnp-org:service:AVTransport:1#Play"
            // 注意：有些控制端会带引号，strstr("#Play") 方式可以兼容该差异。
            const char* soap_action = strstr(buffer, "SOAPACTION:");

            if (soap_action != NULL && strstr(soap_action, "#SetAVTransportURI") != NULL) {
                // SetAVTransportURI 需要从 SOAP Body 中提取 CurrentURI。
                // 这里继续使用轻量字符串截取，减少 MCU 上 XML 解析器依赖。
                char media_url[512] = {0};
                char* uri_start = strstr(buffer, "<CurrentURI>");
                if (uri_start != NULL) {
                    uri_start += strlen("<CurrentURI>");
                    char* uri_end = strstr(uri_start, "</CurrentURI>");
                    if (uri_end != NULL) {
                        int uri_len = uri_end - uri_start;
                        // 严格做边界检查，防止越界拷贝到 media_url。
                        if (uri_len > 0 && uri_len < (int)sizeof(media_url) - 1) {
                            strncpy(media_url, uri_start, uri_len);
                            media_url[uri_len] = '\0';
                        }
                    }
                }

                // 对应动作的 SOAP 成功应答。
                // 使用静态常量字符串，避免每次请求都在栈上构造大块临时文本。
                static const char kSetUriRsp[] =
                    "HTTP/1.1 200 OK\r\n"
                    "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                    "CONNECTION: close\r\n\r\n"
                    "<?xml version=\"1.0\"?>"
                    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<s:Body>"
                    "<u:SetAVTransportURIResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                    "</s:Body>"
                    "</s:Envelope>";
                lwip_send(client_sock, (const uint8_t*)kSetUriRsp, sizeof(kSetUriRsp) - 1, 0);

                // 实际工程里可在这里把 media_url 投递到音频线程队列。
                osal_printk("收到SetAVTransportURI，URL=%s\n", media_url);
            } else if (soap_action != NULL && strstr(soap_action, "#Play") != NULL) {
                // Play 动作应答
                static const char kPlayRsp[] =
                    "HTTP/1.1 200 OK\r\n"
                    "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                    "CONNECTION: close\r\n\r\n"
                    "<?xml version=\"1.0\"?>"
                    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<s:Body>"
                    "<u:PlayResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                    "</s:Body>"
                    "</s:Envelope>";
                lwip_send(client_sock, (const uint8_t*)kPlayRsp, sizeof(kPlayRsp) - 1, 0);
            } else if (soap_action != NULL && strstr(soap_action, "#Pause") != NULL) {
                // Pause 动作应答
                static const char kPauseRsp[] =
                    "HTTP/1.1 200 OK\r\n"
                    "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                    "CONNECTION: close\r\n\r\n"
                    "<?xml version=\"1.0\"?>"
                    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<s:Body>"
                    "<u:PauseResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                    "</s:Body>"
                    "</s:Envelope>";
                lwip_send(client_sock, (const uint8_t*)kPauseRsp, sizeof(kPauseRsp) - 1, 0);
            } else if (soap_action != NULL && strstr(soap_action, "#Stop") != NULL) {
                // Stop 动作应答
                static const char kStopRsp[] =
                    "HTTP/1.1 200 OK\r\n"
                    "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                    "CONNECTION: close\r\n\r\n"
                    "<?xml version=\"1.0\"?>"
                    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                    "<s:Body>"
                    "<u:StopResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
                    "</s:Body>"
                    "</s:Envelope>";
                lwip_send(client_sock, (const uint8_t*)kStopRsp, sizeof(kStopRsp) - 1, 0);
            } else {
                // 已命中 controlURL，但动作未实现，返回 501 更利于控制端判断
                // 语义：资源存在，但服务器不支持该方法/动作。
                static const char k501[] =
                    "HTTP/1.1 501 Not Implemented\r\n"
                    "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                    "CONNECTION: close\r\n\r\n";
                lwip_send(client_sock, (const uint8_t*)k501, sizeof(k501) - 1, 0);
            }
        }
    }

    // ===== 默认情形：未匹配到已知接口 =====
    else {
        // 非 GET 描述、非 POST 控制，且不在已支持协议范围内：返回 400。
        static const char k400[] = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        lwip_send(client_sock, (const uint8_t*)k400, sizeof(k400) - 1, 0);
    }
    
    // 服务完此条指令后即刻切断连接
    lwip_close(client_sock);
}
```

**D. Socket 初始化的正确实现（核心难点详解）**

这里是最容易踩坑的地方！很多人在 SSDP UDP 多播和 HTTP TCP 绑定上犯错误，导致无法收到数据或接受连接。

```cpp
// 专门配置 SSDP UDP 多播套接字的正确方式
int DlnaDmr::setup_ssdp_socket() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        osal_printk("SSDP Socket 创建失败\n");
        return -1;
    }

    // 【第一步】绑定到本机任意地址 + SSDP 端口
    // 关键误区：千万不要 bind 到多播地址 239.255.255.250！
    // 应该 bind 到 INADDR_ANY，表示"接收所有进来的数据"
    sockaddr_in ssdp_addr = {0};
    ssdp_addr.sin_family = AF_INET;
    ssdp_addr.sin_addr.s_addr = INADDR_ANY;  // ← 必须是 INADDR_ANY!
    ssdp_addr.sin_port = htons(SSDP_PORT);   // 1900

    errcode_t ret = bind(sock, (sockaddr *)&ssdp_addr, sizeof(ssdp_addr));
    if (ret != 0) {
        osal_printk("SSDP bind 失败\n");
        lwip_close(sock);
        return -1;
    }

    // 【第二步】加入多播组（这是接收多播包的必要且充分条件！）
    // 不加入多播组，即使 bind 了，也根本收不到 239.255.255.250 的多播包
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MCAST_IP);  // 多播地址 239.255.255.250
    mreq.imr_interface.s_addr = INADDR_ANY;            // 接收多播的本地网卡（任意网卡都可以）

    ret = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq));
    if (ret != 0) {
        osal_printk("加入多播组失败\n");
        lwip_close(sock);
        return -1;
    }

    osal_printk("SSDP Socket 初始化成功\n");
    return sock;
}

// 专门配置 HTTP TCP 服务器套接字的正确方式
int DlnaDmr::setup_http_socket() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        osal_printk("HTTP Socket 创建失败\n");
        return -1;
    }

    // 【第一步】绑定到本机任意地址 + HTTP 端口
    sockaddr_in http_addr = {0};
    http_addr.sin_family = AF_INET;
    http_addr.sin_addr.s_addr = INADDR_ANY;
    http_addr.sin_port = htons(HTTP_PORT);  // 49152

    errcode_t ret = bind(sock, (sockaddr *)&http_addr, sizeof(http_addr));
    if (ret != 0) {
        osal_printk("HTTP bind 失败\n");
        lwip_close(sock);
        return -1;
    }

    // 【第二步】调用 listen() 让套接字进入被动接受状态
    // 这是 TCP 服务器的必须步骤！很多新手忘的就是这一行
    // 参数 5 表示："最多同时有 5 个未被 accept 的连接请求排队等待"
    ret = listen(sock, 5);
    if (ret != 0) {
        osal_printk("HTTP listen 失败\n");
        lwip_close(sock);
        return -1;
    }

    osal_printk("HTTP Socket 初始化成功，监听端口 %d\n", HTTP_PORT);
    return sock;
}
```

**关键排坑总结：**

| 错误类型 | 常见现象 | 正确做法 |
|---------|---------|---------|
| **SSDP** | bind 到多播地址 | ✅ bind 到 INADDR_ANY，然后 `setsockopt(IP_ADD_MEMBERSHIP)` |
| **SSDP** | 没有加入多播组 | ✅ 必须调用 `setsockopt(IPPROTO_IP, IP_ADD_MEMBERSHIP, ...)` |
| **HTTP** | bind 之后直接 recv | ❌ 缺少 `listen()` 步骤，无法接受连接 |
| **HTTP** | 无法 accept 客户端连接 | ✅ bind 之后一定要 `listen(sock, 5)` |


---

## 四、 关键排坑指南（结合现状）

1.  **Socket 配置大坑：**
    - **SSDP UDP 多播错误：** 绝对不能 `bind()` 到多播地址 239.255.255.250！正确做法是先 `bind()` 到 `INADDR_ANY:1900`，然后用 `setsockopt(IPPROTO_IP, IP_ADD_MEMBERSHIP, ...)` 加入多播组。否则你永远收不到手机的 M-SEARCH 搜索包。
    - **HTTP TCP 服务器不完整：** `bind()` 之后必须立刻调用 `listen(sock, 5)` 让套接字进入被动接受连接的状态。忘记这一行，客户端连接会被拒绝。

2.  **修复 Socket API 报错：** 在你的 `wifi.cpp` 的 `407` 行等位置，提示了 `undefined reference to 'close'`。在 LwIP / LiteOS 环境下，请**务必**将所有的 `close(sfd)` 替换为 `lwip_close(sfd)`。

3.  **避免在回调中阻塞：** `wifi` 类或者 `DLNA` 的网络通信回调（包括 `wifi_connection_changed_callback`）由于要在短时间内给协议栈让出资源，不要在网络读取/监听上使用无限的 `while(1) + sleep` 堵死系统 Task。务必合理使用 Osal Semaphore 和独立新建 Task（如上述 `osal_task_create` ）。

4.  **内存管理：** 因为 WS63 内存并非无限大，解析 HTTP Header 和 XML 时，不要随便在函数里申请巨大的局部数组（如 `char buffer[16384]`），而是使用 `osal_kmalloc` 动态申请并在结束时 `osal_kfree`，或者严格控制 buffer 大小在 1K\~4K 这个量级（对于控制指令来说绝对够用了）。

5.  **音频 MP3 解码流程：** 获取到 MP3 网址后，发起 TCP 建立 HTTP 数据下载流 `recv(...)`。不要等全部下完（因为内存装不下整首歌），应该是**边下边播**。分配一个约 8KB 的环形缓冲区（Ring Buffer），网络存，`minimp3` 解码出 1152 个 PCM 采样，就往 I2S 硬件缓冲推入，往复循环即可达到即刻发声的效果！

---

## 五、 UPnP 事件订阅机制（反向控制 - 音响主动通知手机）

当前的单向架构只能让手机控制音响（Play、Pause、Stop 等）。但如果想让音响反向通知手机播放进度、音量变化等信息，需要实现 **UPnP Eventing** 机制。

### 核心概念：SUBSCRIBE - NOTIFY - UNSUBSCRIBE

```text
手机 (DMC)          音响 (DMR)
  |                   |
  |---- SUBSCRIBE ---->|  (手机：我要订阅你的播放状态变化)
  |                   |
  |<---- 200 OK ------|  (音响：好的，已注册你的回调地址)
  |                   |
  [用户按暂停键]       |
  |                   |
  |<---- NOTIFY ------|  (音响：我的状态改变了！现在是 STOPPED)
  |                   |
  |---- 200 OK ------>|  (手机：收到！)
```

### E. HTTP 处理扩展：支持 SUBSCRIBE 和 UNSUBSCRIBE

在 `handle_http_request()` 中添加事件订阅处理：

```cpp
// 需要在 DlnaDmr 类中添加订阅者管理
struct Subscriber {
    char callback_ip[16];      // 手机的 IP（从 CALLBACK 头提取）
    uint16_t callback_port;    // 手机的端口（通常 8080）
    char sid[64];              // Subscription ID（唯一标识）
    uint32_t seq_num;          // 事件序列号（每次 NOTIFY 递增）
    uint32_t subscribe_time;   // 订阅时间戳（用于超时判断）
};

// WS63 内存限制，只支持 1-2 个并发订阅
static constexpr int MAX_SUBSCRIBERS = 2;
Subscriber subscribers[MAX_SUBSCRIBERS];
int subscriber_count = 0;

// ========== 主要的 HTTP 请求路由 ==========
void DlnaDmr::handle_http_request(int server_sock) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
    if (client_sock < 0) return;

    char buffer[1024];
    int len = recv(client_sock, buffer, sizeof(buffer)-1, 0);
    if (len <= 0) {
        lwip_close(client_sock);
        return;
    }
    buffer[len] = '\0';
    std::string req(buffer);
    
    // 【新增】处理 SUBSCRIBE 订阅请求
    if (req.find("SUBSCRIBE") != std::string::npos) {
        handle_subscribe_request(req, client_addr.sin_addr.s_addr, client_sock);
    }
    // 【新增】处理 UNSUBSCRIBE 取消订阅请求
    else if (req.find("UNSUBSCRIBE") != std::string::npos) {
        handle_unsubscribe_request(req, client_sock);
    }
    // 【源有】处理获取设备描述 XML
    else if (req.find("GET /description.xml") != std::string::npos) {
        const char* xml_desc = 
            "HTTP/1.1 200 OK\r\n"
            "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
            "CONTENT-LENGTH: 1024\r\n\r\n"
            "<?xml version=\"1.0\"?>"
            "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
            // ... （设备描述信息，省略细节）
            "</root>";
        send(client_sock, xml_desc, strlen(xml_desc), 0);
    }
    // 【源有】处理 SetAVTransportURI 播放指令
    else if (req.find("POST") != std::string::npos && 
             req.find("SetAVTransportURI") != std::string::npos) {
        std::string media_url = extract_xml_tag(req, "CurrentURI");
        osal_printk("拿到歌曲地址: %s\n", media_url.c_str());
        
        // 发送 200 OK SOAP 响应
        const char* ok_response = 
            "HTTP/1.1 200 OK\r\n"
            "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n\r\n"
            "<?xml version=\"1.0\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body><u:SetAVTransportURIResponse /></s:Body>"
            "</s:Envelope>";
        send(client_sock, ok_response, strlen(ok_response), 0);
    }
    
    lwip_close(client_sock);
}

// ========== SUBSCRIBE 处理：手机订阅事件 ==========
void DlnaDmr::handle_subscribe_request(const std::string& req, uint32_t client_ip, int client_sock) {
    // 从请求头中提取 CALLBACK 地址
    // CALLBACK 头格式如：<http://192.168.1.100:8080/>
    size_t callback_start = req.find("CALLBACK: <");
    if (callback_start == std::string::npos) {
        send(client_sock, "HTTP/1.1 400 Bad Request\r\n\r\n", 28, 0);
        return;
    }
    
    callback_start += 11;  // 跳过 "CALLBACK: <"
    size_t callback_end = req.find(">", callback_start);
    std::string callback_url = req.substr(callback_start, callback_end - callback_start);
    
    // 简化处理：从 callback_url 提取 IP 和端口
    // 实际需要 URL 解析，这里省略细节
    uint16_t callback_port = 8080;  // 通常是这个端口
    
    // 检查是否已满
    if (subscriber_count >= MAX_SUBSCRIBERS) {
        // 覆盖最旧的订阅（FIFO 策略）
        subscriber_count = 1;
    }
    
    // 生成唯一的 SID（Subscription ID）
    char sid[64];
    snprintf(sid, sizeof(sid), "uuid:%08x-%04x-%04x-%04x-%lu",
             (unsigned int)(client_ip & 0xFFFFFFFF),
             (unsigned int)(osal_get_ticks() & 0xFFFF),
             (unsigned int)((osal_get_ticks() >> 16) & 0xFFFF),
             (unsigned int)((osal_get_ticks() >> 32) & 0xFFFF),
             (unsigned long)subscriber_count);
    
    // 存储本订阅
    Subscriber& sub = subscribers[subscriber_count];
    snprintf(sub.callback_ip, sizeof(sub.callback_ip), "%d.%d.%d.%d",
             (client_ip >> 24) & 0xFF,
             (client_ip >> 16) & 0xFF,
             (client_ip >> 8) & 0xFF,
             client_ip & 0xFF);
    sub.callback_port = callback_port;
    strncpy(sub.sid, sid, sizeof(sub.sid)-1);
    sub.seq_num = 0;
    sub.subscribe_time = osal_get_ticks();
    subscriber_count++;
    
    // 回复 200 OK + SID + TIMEOUT
    const char* response_template =
        "HTTP/1.1 200 OK\r\n"
        "SID: %s\r\n"
        "TIMEOUT: Second-1800\r\n"
        "CONTENT-LENGTH: 0\r\n\r\n";
    
    char response[256];
    snprintf(response, sizeof(response), response_template, sid);
    send(client_sock, response, strlen(response), 0);
    
    osal_printk("新订阅已注册: %s (SID: %s)\n", sub.callback_ip, sid);
}

// ========== UNSUBSCRIBE 处理：手机取消订阅 ==========
void DlnaDmr::handle_unsubscribe_request(const std::string& req, int client_sock) {
    // 从请求头中提取 SID
    size_t sid_start = req.find("SID:");
    if (sid_start == std::string::npos) {
        send(client_sock, "HTTP/1.1 400 Bad Request\r\n\r\n", 28, 0);
        return;
    }
    
    sid_start += 5;  // 跳过 "SID: "
    size_t sid_end = req.find("\r\n", sid_start);
    std::string sid = req.substr(sid_start, sid_end - sid_start);
    
    // 在订阅列表中查找并删除
    for (int i = 0; i < subscriber_count; i++) {
        if (strcmp(subscribers[i].sid, sid.c_str()) == 0) {
            // 删除这个订阅（向前移动其他的）
            for (int j = i; j < subscriber_count - 1; j++) {
                subscribers[j] = subscribers[j + 1];
            }
            subscriber_count--;
            osal_printk("订阅已取消: %s\n", sid.c_str());
            break;
        }
    }
    
    // 回复 200 OK
    send(client_sock, "HTTP/1.1 200 OK\r\nCONTENT-LENGTH: 0\r\n\r\n", 40, 0);
}

// ========== 主动推送：当播放状态变化时 ==========
void DlnaDmr::notify_subscribers_state_changed(const char* new_state) {
    // 例如 new_state = "PLAYING" 或 "STOPPED" 或 "PAUSED_PLAYBACK"
    
    for (int i = 0; i < subscriber_count; i++) {
        Subscriber& sub = subscribers[i];
        
        // 【关键】创建新的 TCP 连接指向手机的回调地址
        int notify_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (notify_sock < 0) continue;
        
        struct sockaddr_in phone_addr = {0};
        phone_addr.sin_family = AF_INET;
        phone_addr.sin_port = htons(sub.callback_port);
        phone_addr.sin_addr.s_addr = inet_addr(sub.callback_ip);
        
        if (connect(notify_sock, (struct sockaddr*)&phone_addr, sizeof(phone_addr)) < 0) {
            lwip_close(notify_sock);
            continue;
        }
        
        // 构建 NOTIFY 消息
        const char* notify_header =
            "NOTIFY /callback HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
            "CONTENT-LENGTH: %d\r\n"
            "NT: upnp:event\r\n"
            "NTS: upnp:propchange\r\n"
            "SID: %s\r\n"
            "SEQ: %u\r\n"
            "CONNECTION: close\r\n\r\n";
        
        const char* notify_body =
            "<?xml version=\"1.0\"?>"
            "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
            "<e:property><TransportState>%s</TransportState></e:property>"
            "</e:propertyset>";
        
        // 格式化消息体
        char body[512];
        int body_len = snprintf(body, sizeof(body), notify_body, new_state);
        
        // 格式化消息头
        char header[512];
        int header_len = snprintf(header, sizeof(header), notify_header,
                                  sub.callback_ip, sub.callback_port,
                                  body_len,
                                  sub.sid,
                                  sub.seq_num);
        
        // 发送完整 NOTIFY
        send(notify_sock, header, header_len, 0);
        send(notify_sock, body, body_len, 0);
        
        sub.seq_num++;  // 序列号递增
        lwip_close(notify_sock);
        
        osal_printk("已通知 %s: 新状态 = %s\n", sub.callback_ip, new_state);
    }
}

// 【在 audio_player_task 中的用法示例】
// 当用户按下暂停键时，调用：
// dmr->notify_subscribers_state_changed("PAUSED_PLAYBACK");
```

### 内存优化策略对比

| 方案 | 实现复杂度 | 内存开销 | 适用场景 | 性能 |
|------|----------|---------|---------|------|
| **无 Eventing**<br>（现在的） | ⭐ 低 | ~0 | 纯单向投屏 | 最快 |
| **一个订阅**<br>（推荐 WS63） | ⭐⭐ 中 | ~200B | 一部手机控制 | 很好 |
| **两个订阅**<br>（本代码） | ⭐⭐ 中 | ~400B | 多个手机同时控制 | 很好 |
| **完整 Event List**<br>（标准 UPnP） | ⭐⭐⭐ 高 | ~2-5KB | 工业应用 | 标准兼容 |

**WS63 推荐方案：** 一个订阅（最新的手机覆盖旧订阅），总内存 ~200B，足够满足日常投屏需求。

---

## 六、 从“基础框架”到“完美兼容”：重点缺失功能补全指南 

根据系统现状分析，当前代码已经实现了 DLNA 音响的**基本框架**（SSDP M-SEARCH 响应、设备描述 XML、HTTP 接收 TCP 连接框架）。但这对于标准 DLNA 协议来说仍然是不完整的，很多主流 App（QQ音乐、网易云等）会因为缺少**服务描述文件**或**状态查询接口**而投屏失败或断开连接。

以下是针对当前架构缺失环节的详细补全路线图，按开发优先级排序：

### 1. 【高优先级】添加服务描述文件 (Service XMLs)

**问题原因**：控制端通过 `/description.xml` 知道了设备包含 `AVTransport` (传输控制) 和 `RenderingControl` (渲染控制) 服务后，会紧接着去请求对应的 `.xml` 来阅读你的接口规范。如果没有这三个文件，大部分 DMC 会直接停止通信。

**实现方法**：在 `handle_http_request` 函数的 `GET` 分支中补充路径响应：

```cpp
// 在 HTTP 处理的 GET 请求部分追加：
    if (strstr(buffer.data(), "GET /AVTransport.xml") != nullptr || strstr(buffer.data(), "HEAD /AVTransport.xml") != nullptr) {
        osal_printk("http收到 AVTransport.xml 请求\n");
        static constexpr const char* avt_xml = 
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
            "  <actionList>\r\n"
            "    <action><name>SetAVTransportURI</name></action>\r\n"
            "    <action><name>Play</name></action>\r\n"
            "    <action><name>Pause</name></action>\r\n"
            "    <action><name>Stop</name></action>\r\n"
            "    <action><name>GetTransportInfo</name></action>\r\n"
            "    <action><name>GetPositionInfo</name></action>\r\n"
            "  </actionList>\r\n"
            "</scpd>";
        
        std::array<char, 256> header;
        int response_length = strlen(avt_xml);
        snprintf(header.data(), header.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "Server: OS/1.0 UPnP/1.1 product/1.0\r\n\r\n",
                 response_length);
        lwip_send(client_sock, header.data(), strlen(header.data()), 0);
        lwip_send(client_sock, (const uint8_t*)avt_xml, response_length, 0);
    }
    else if (strstr(buffer.data(), "GET /RenderingControl.xml") != nullptr || strstr(buffer.data(), "HEAD /RenderingControl.xml") != nullptr) {
        osal_printk("http收到 RenderingControl.xml 请求\n");
        static constexpr const char* rc_xml = 
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
            "  <actionList>\r\n"
            "    <action><name>SetVolume</name></action>\r\n"
            "    <action><name>GetVolume</name></action>\r\n"
            "    <action><name>SetMute</name></action>\r\n"
            "    <action><name>GetMute</name></action>\r\n"
            "  </actionList>\r\n"
            "</scpd>";
            
        std::array<char, 256> header;
        int response_length = strlen(rc_xml);
        snprintf(header.data(), header.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "Server: OS/1.0 UPnP/1.1 product/1.0\r\n\r\n",
                 response_length);
        lwip_send(client_sock, header.data(), strlen(header.data()), 0);
        lwip_send(client_sock, (const uint8_t*)rc_xml, response_length, 0);
    }
    else if (strstr(buffer.data(), "GET /ConnectionManager.xml") != nullptr || strstr(buffer.data(), "HEAD /ConnectionManager.xml") != nullptr) {
        osal_printk("http收到 ConnectionManager.xml 请求\n");
        static constexpr const char* cm_xml = 
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\r\n"
            "  <actionList>\r\n"
            "    <action><name>GetProtocolInfo</name></action>\r\n"
            "    <action><name>GetCurrentConnectionIDs</name></action>\r\n"
            "  </actionList>\r\n"
            "</scpd>";
            
        std::array<char, 256> header;
        int response_length = strlen(cm_xml);
        snprintf(header.data(), header.size(),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "Server: OS/1.0 UPnP/1.1 product/1.0\r\n\r\n",
                 response_length);
        lwip_send(client_sock, header.data(), strlen(header.data()), 0);
        lwip_send(client_sock, (const uint8_t*)cm_xml, response_length, 0);
    }
```

*注：严格的 UPnP 规范中，这些 XML 还需包含 `serviceStateTable`（变量表，详细可查阅 UPnP 官方文档），但很多通用播放器只要看到动作声明通过即可放行。如果有更严格的播放器，可以按照标准补充完整的 StateVariable。*

### 2. 【中优先级】完善 AVTransport 查询接口 (显示播放状态与进度)

**问题原因**：除了被动接收 `Play/Pause` 指令，手机音乐 App 会高频发送 SOAP 请求询问“你目前播放进度多少了”、“当前是播放还是停止状态”。如果音响不回应 `GetPositionInfo`，手机端进度条就完全死机，且有些手机可能会认为音响罢工而主动切断控制逻辑。

**实现方法**：在处理对应的 `SOAPACTION` 时返回状态数据：

```cpp
// 1. 获取传输状态 (GetTransportInfo)
else if (strstr(buffer.data(), "POST") != nullptr && strstr(buffer.data(), "#GetTransportInfo") != nullptr) {
    // 当前播放状态需要从全局变量或内部音频状态机中获取
    // 必须为规范字符串: PLAYING, PAUSED_PLAYBACK, STOPPED, NO_MEDIA_PRESENT 等
    const char* current_state = "PLAYING"; // 示例硬编码，实际应根据硬件发声状态动态改变
    
    char response[512];
    char header[256];
    
    int body_len = snprintf(response, sizeof(response),
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body>"
        "<u:GetTransportInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<CurrentTransportState>%s</CurrentTransportState>"
        "<CurrentTransportStatus>OK</CurrentTransportStatus>"
        "<CurrentSpeed>1</CurrentSpeed>"
        "</u:GetTransportInfoResponse>"
        "</s:Body></s:Envelope>", current_state);
        
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/xml; charset=\"utf-8\"\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n",
             body_len);
             
    lwip_send(client_sock, header, strlen(header), 0);
    lwip_send(client_sock, (const uint8_t*)response, body_len, 0);
}

// 2. 获取进度信息 (GetPositionInfo)
else if (strstr(buffer.data(), "POST") != nullptr && strstr(buffer.data(), "#GetPositionInfo") != nullptr) {
    // 这两个时长需要从音频解码器 (minimp3) 那里去获取并计算
    // 格式必须严格为 HH:MM:SS (例如 00:01:23)
    const char* track_duration = "00:04:30"; // 音乐总长
    const char* rel_time = "00:01:15";       // 当前已播进度
    
    char response[768];
    char header[256];
    
    int body_len = snprintf(response, sizeof(response),
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body>"
        "<u:GetPositionInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<Track>1</Track>"
        "<TrackDuration>%s</TrackDuration>"
        "<TrackMetaData></TrackMetaData>"
        "<TrackURI></TrackURI>"
        "<RelTime>%s</RelTime>"
        "<AbsTime>%s</AbsTime>"
        "<RelCount>2147483647</RelCount>"
        "<AbsCount>2147483647</AbsCount>"
        "</u:GetPositionInfoResponse>"
        "</s:Body></s:Envelope>", track_duration, rel_time, rel_time);
        
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/xml; charset=\"utf-8\"\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n",
             body_len);
             
    lwip_send(client_sock, header, strlen(header), 0);
    lwip_send(client_sock, (const uint8_t*)response, body_len, 0);
}
```

*注：除了上面这两个，后续还应支持 `Seek` 指令，用于处理手机端拖动进度条。*

### 3. 【中优先级】完善 RenderingControl 实现 (音量与静音)

**问题原因**：这是提供基础体验的一环。如果不具备此功能，手机侧无法正常调节音效，按动音量键往往无反应。
 
**实现方法**：在判断路径为 `POST /RenderingControl/control` 的分支中处理。

```cpp
const bool is_rc_control = (strstr(buffer.data(), "POST /RenderingControl/control") != nullptr);
if (is_rc_control) {
    const char* soap_action_start = strstr(buffer.data(), "SOAPACTION:");
    if (soap_action_start != nullptr) {
        // 1. 手机修改音响音量
        if (strstr(soap_action_start, "#SetVolume") != nullptr) {
            std::array<char, 32> desired_vol = {0};
            extract_xml_tag_value(buffer.data(), "DesiredVolume", desired_vol.data(), desired_vol.size());
            int vol = atoi(desired_vol.data());
            // TODO: 调用芯片硬件音量调节接口或 I2S 增益调节
            // hal_audio_set_volume(vol); 
            
            static const char* ok_response = 
                "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<s:Body><u:SetVolumeResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\"/></s:Body>"
                "</s:Envelope>";
                
            char header[256];
            snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n\r\n",
                     (int)strlen(ok_response));
                     
            lwip_send(client_sock, header, strlen(header), 0);
            lwip_send(client_sock, (const uint8_t*)ok_response, strlen(ok_response), 0);
        }
        // 2. 手机查询当前音量
        else if (strstr(soap_action_start, "#GetVolume") != nullptr) {
            int current_vol = 50; // TODO: 应该返回实际底层音量 (0~100)
            char response[384];
            char header[256];
            
            int body_len = snprintf(response, sizeof(response),
                "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<s:Body><u:GetVolumeResponse xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
                "<CurrentVolume>%d</CurrentVolume>"
                "</u:GetVolumeResponse></s:Body></s:Envelope>", current_vol);
                
            snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n\r\n",
                     body_len);
                     
            lwip_send(client_sock, header, strlen(header), 0);
            lwip_send(client_sock, (const uint8_t*)response, body_len, 0);
        }
    }
}
```

### 4. 【系统联调建议总结】

1. **统一数据状态机**：不要把所有控制和查询强行写在不同的独立回调中，最好维护一个全局/类级结构体变量：如 `struct AudioState { int volume; std::string status; int duration_sec; int current_sec; };` ，HTTP 解析只负责返回这些变量，而后台的播放器 Task 负责刷新这些变量，维持状态的一致性。
2. **巧妙使用 Wireshark 抓包辅助**：在 PC 连接同一局域网并打开 Wireshark ，过滤 `ip.addr == [手机IP]` 或 `http`。当你点击手机进行投屏时，你会直接看到“哪一步卡住了”、“手机发了什么但音响没回复”。这是解决 DLNA 握手失败、掉线的终极秘籍。

---

## 七、 关键细节答疑

### Q1: 为什么需要 `int max_fd = (dmr->ssdp_sock > dmr->http_sock) ? dmr->ssdp_sock : dmr->http_sock;` 这一步？

**关键理由：`select()` 的第一个参数需要指定"最大的文件描述符+1"。**

```c
int select(int nfds,        // ← 必须是：max_fd + 1
           fd_set *readfds,
           fd_set *writefds,
           fd_set *exceptfds,
           struct timeval *timeout);
```

#### 为什么要 +1？

文件描述符在 POSIX 中是从 `0` 开始的整数编号：
- 文件描述符范围：`0, 1, 2, 3, ..., max_fd`
- 总共需要检查的个数：`max_fd + 1` 个

**例如：**
- 如果 `ssdp_sock = 3`，`http_sock = 5`
- `max_fd = 5`
- `select(max_fd + 1, ...)` → `select(6, ...)`  ← **检查 fd 0,1,2,3,4,5 共 6 个**

#### 为什么 select() 需要这个信息？

`select()` 在内核中需要遍历 `fd_set` 位数组，从 `0` 扫描到 `nfds-1`。如果你告诉它 `nfds=100` 但实际最大 fd 只有 5，会浪费 CPU 检查无效的范围。如果你告诉它 `nfds=3` 但有 fd=5，那个 fd 会被忽略！

#### LwIP 中的实际例子

```cpp
int ssdp_sock = 10;   // 第一个 socket，fd=10
int http_sock = 12;   // 第二个 socket，fd=12

int max_fd = (ssdp_sock > http_sock) ? ssdp_sock : http_sock;  // max_fd = 12

fd_set read_fds;
FD_ZERO(&read_fds);
FD_SET(ssdp_sock, &read_fds);   // 监控 fd 10
FD_SET(http_sock, &read_fds);   // 监控 fd 12

// 【错误❌】select(3, &read_fds, ...);
// 问题：只检查 fd 0,1,2，忽略了 fd 10 和 12！select 会立刻返回 0（超时）

// 【正确✅】select(max_fd + 1, &read_fds, ...);
// 正确：select(13, ...)，检查 fd 0 到 12，正确监控了 fd 10 和 12
```

#### 为什么不直接用一个大数字，如 `select(1024, ...)`？

虽然技术上可行，但会浪费内核资源。内核会无谓地扫描 0~1023，其中大部分未使用。最佳实践就是使用实际的最大 fd+1。

#### 【实际验证】传错 nfds 参数的灾难后果

```cpp
// 【场景1：传太小的 nfds】
int ssdp_sock = 10;
int http_sock = 12;

fd_set read_fds;
FD_ZERO(&read_fds);
FD_SET(ssdp_sock, &read_fds);
FD_SET(http_sock, &read_fds);

// ❌ 错误：select(5, &read_fds, ...);
// 结果：手机怎么发送M-SEARCH都收不到！
//       因为 select 只会检查 fd 0~4，
//       忽略了 fd 10 和 12
//       手机永远看不到音响设备

while (dmr->is_running) {
    int ret = select(5, &read_fds, NULL, NULL, NULL);  // ← BUG!
    
    if (ret > 0) {
        // 这个分支几乎永远不会进入
        // 因为 select 根本没在监听正确的 fd
        if (FD_ISSET(dmr->ssdp_sock, &read_fds)) {
            osal_printk("收到M-SEARCH"); // ← 永远不会被打印！
        }
    }
}

// ---

// 【场景2：传太大的 nfds】  
// ✅ select(1024, &read_fds, ...);  // 虽然能工作，但...
// 问题：内核需要检查 fd 0 到 1023，其中 1008 个是无用的！
//       在嵌入式系统上，这会明显拖累响应速度
//       每次 select() 都做无谓的工作，CPU 频繁唤醒
//       在 WS63 这种低功耗芯片上，会严重影响能耗

// 【正确✅】
int max_fd = (ssdp_sock > http_sock) ? ssdp_sock : http_sock;
int ret = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
// 现在内核只检查 fd 0 到 12，13 个而已，非常高效
```

#### 在 WS63 上为什么这个优化很重要？

- **内存有限**（只有 549KB SRAM）
- **处理能力有限**（160MHz CPU）  
- **低功耗要求**（电池续航）

每次 `select()` 都浪费 CPU 去检查 1000 个无效 fd，会导致：
- 中断响应变慢（手机搜索设备要等很久才能收到回复）
- 功耗增加（CPU 频繁处理无关的 fd 检查）

所以 `max_fd` 这个计算虽然只有一行代码，但在嵌入式场景下意义重大。

---

### Q2: 为什么音响需要主动推送 NOTIFY？

在 DLNA/UPnP 中，手机需要实时知道音响的状态（播放进度、音量、是否暂停等）。如果只能手机单向发送请求，不能实时了解动态，就无法正确显示进度条、更新 UI 等。通过 Eventing 机制，音响可以主动告知手机每一状态变化，手机 UI 立刻响应。

---

### Q3: 这些 HTTP 响应字符串很大，能不能用 `static constexpr` 来降低占用？

**可以，但要分清楚它降低的是哪一类占用。**

1. 对于固定不变的字符串模板（如 Header 模板、SOAP 固定 body），使用 `static constexpr char[]` 或 `static const char[]` 很合适：
    - 常量会放在只读段（ROM/Flash），避免运行时重复构造。
    - 多次调用复用同一份常量。

2. 但仅仅加 `static constexpr`，**不会**消掉你当前的两块临时缓冲区：
    - `std::array<char, 2048> xml_response`
    - `std::array<char, 256> header`
    这两块本质上是运行时拼接内容所需的可写内存，依然会占 RAM。

3. 如果目标是进一步降 RAM，推荐优先级如下：
    - 先把“纯常量响应”改为静态只读字符串，直接 `lwip_send()` 发送。
    - 对包含变量字段的 XML，保留模板为 `static constexpr`，再用较小缓冲区分段 `snprintf + send`，避免一次性 2KB 大缓冲。
    - 如果是单线程串行处理连接，可把大缓冲改为函数外 `static` 复用缓冲，减少栈峰值。

4. 一个实用结论：
    - `static constexpr` 对“代码可读性 + ROM复用”收益明显。
    - 真正影响你当前峰值 RAM 的主要是“可写拼包缓冲区大小”，需要通过分段发送或复用缓冲来优化。

---

## 八、基于你当前代码的详细测试方法（可直接执行）

你当前工程已经能稳定完成：发现设备、订阅事件、接收控制命令。下一步测试目标不是“有没有请求进来”，而是“控制端是否持续信任该设备”。下面按阶段给出可执行测试流程。

### 0. 测试前准备

1. 固件开启串口日志，并确认可看到以下关键日志：
    - `dlan启动成功`
    - `http监听已启动`
    - `ssdp收到M-SEARCH`
2. 手机与 WS63 必须在同一网段。
3. 路由器关闭 AP 隔离（否则手机发出的 SSDP 组播可能到不了设备）。
4. 建议准备一台 PC 抓包（Wireshark），过滤器可用：
    - `udp.port == 1900`
    - `http`
    - `ip.addr == 手机IP`

### 1. 阶段A：发现链路测试（SSDP）

目标：验证控制端能持续发现，而不是偶发发现。

步骤：
1. 手机打开 QQ 音乐投屏页，观察 10 秒。
2. 串口应反复出现 `ssdp收到M-SEARCH` 和 `ssdp已响应M-SEARCH`。
3. 在 Wireshark 确认每次 `M-SEARCH` 都能看到设备 `HTTP/1.1 200 OK` 响应。

判定标准：
1. 连续 10 次搜索均有响应，且 `LOCATION` 指向当前设备 IP。
2. `ST` 与请求匹配（尤其是 `MediaRenderer:1`）。

失败定位：
1. 若手机偶尔看不到设备：优先检查组播加入与路由器隔离设置。
2. 若只首次能搜到：检查 `ssdp_sock` 是否被异常关闭。

### 2. 阶段B：握手链路测试（HTTP 描述与订阅）

目标：验证从 `description.xml` 到 `SUBSCRIBE` 的完整握手是否稳定。

步骤：
1. 手机点击你的音响设备。
2. 串口应按顺序出现：
    - `GET /description.xml`
    - `GET /AVTransport.xml`、`GET /RenderingControl.xml`、`GET /ConnectionManager.xml`
    - `SUBSCRIBE /AVTransport/event`
    - `SUBSCRIBE /RenderingControl/event`
3. 订阅响应必须返回 `200 OK`、`SID`、`TIMEOUT`。

判定标准：
1. 上述请求链路完整，且每次都成功。
2. 不出现 `http接受连接失败`、`http数据接收失败` 连续刷屏。

失败定位：
1. 若订阅后立即掉回手机：通常是后续控制查询动作未通过（进入阶段C定位）。

### 3. 阶段C：控制动作与状态一致性测试（当前最关键）

目标：避免“瞬间选中后回退手机”。

说明：
你当前已经实现了 `SetAVTransportURI/Play/GetVolume/GetProtocolInfo` 等动作，但播放状态和进度还未真实联动。控制端在这一阶段会做一致性校验。

步骤：
1. 手机点击播放，串口应出现：
    - `SetAVTransportURI`
    - `Play`
    - 紧接着多次查询类动作（如 `GetTransportInfo`、`GetPositionInfo`、`GetVolume`）
2. 检查所有查询动作是否都返回 `200` 而非 `501`。
3. 检查返回值之间是否逻辑一致：
    - 若返回 `PLAYING`，`RelTime` 应能递增。
    - 若返回 `PAUSED_PLAYBACK`，`RelTime` 应冻结。

判定标准：
1. 点击播放后 30 秒内，手机端不自动切回本机扬声器。
2. 控制端进度条不长期卡死在单一时间点。

失败定位：
1. 若出现无具体动作日志的 `POST /AVTransport/control`：通常是 SOAPACTION 匹配失败或收到未实现动作。
2. 若动作都 200 仍回退：通常是状态机不一致或无事件通知。

### 4. 阶段D：长稳测试（回归）

目标：验证不是“短时可用”。

步骤：
1. 连续执行 20 次连接/断开。
2. 每次都执行一次播放、暂停、继续、停止。
3. 观察内存和句柄是否增长（重点排查 socket 泄漏）。

判定标准：
1. 20 轮后仍可正常连接。
2. 不出现 `accept` 失败累积。

### 5. 阶段E：建议加入的自动化冒烟脚本（可选）

可以用 PC 对设备控制口发送固定 SOAP 报文，自动验证：
1. 返回码是否始终为 `200` 或预期 `501`。
2. 响应体是否包含关键标签（如 `CurrentTransportState`）。
3. 每次请求后连接都正常关闭。

这样可以在不依赖手机 UI 的情况下快速回归。

---

## 九、minimp3 适配说明（针对 WS63 + 你当前架构）

你提到“minimp3 似乎有很多支持”，这个理解是对的。对 MCU 项目最关键的是：接口形态、编译特性、以及和你的网络/I2S线程如何拼接。

### 1. minimp3 常见能力怎么选

minimp3 常见用法可以分两类：
1. 基础解码接口：你喂入一段 MP3 字节流，它吐出 PCM。
2. 扩展接口（带文件/流包装）：更偏桌面或文件系统场景。

对你当前 WS63 场景（网络流 + ring buffer）推荐：
1. 优先使用基础解码接口。
2. 自己控制网络接收、缓存、喂数节奏。
3. 避免重依赖文件系统封装层。

### 2. 适配总原则（与你当前 DLNA 线程模型匹配）

你现在是：
1. DLNA 主任务：`select` 监听 SSDP/HTTP。
2. 播放任务：接收 URL 后进行音频下载和解码。

建议保持：
1. `SetAVTransportURI` 只做“更新 URL + 通知播放线程”。
2. 不在 HTTP 处理线程里做解码或长时间下载。
3. 播放线程负责：HTTP GET 拉流 -> ring buffer -> minimp3 -> I2S。

### 3. 最小接入步骤（工程层面）

1. 将 `minimp3.h` 放入项目三方目录（例如 open source 目录下单独子目录）。
2. 新增一个专用实现文件（例如音频模块实现文件）并在其中只定义一次实现宏：

```cpp
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"
```

3. 其它使用处仅 `#include "minimp3.h"`，不要重复定义实现宏。
4. CMake 中把该实现文件加入编译目标，并把头文件目录加入 include path。

说明：
1. `MINIMP3_NO_SIMD` 适合 MCU（避免平台 SIMD 相关路径）。
2. 若后续需要浮点输出再评估其它编译选项，首版先走默认 16-bit PCM 更稳妥。

### 4. 网络流解码的关键细节（首版必须做）

1. 分块接收：每次 `recv` 取 1KB~4KB，写入 ring buffer。
2. 解码循环：每次从 ring buffer 取可用数据喂给解码器，输出 PCM 帧后立刻推 I2S。
3. 欠载处理：当网络抖动导致数据不足时，输出静音或短等待，避免爆音。
4. 进度计算：
    - 已播放样本数累计为 `played_samples`。
    - 当前秒数可按 $played\_samples / sample\_rate$ 估算。
    - 用该值更新 `GetPositionInfo` 的 `RelTime`。

### 5. 与 DLNA 状态机联动（避免被控制端回退）

建议增加一个统一状态结构（类静态或全局受保护对象）：

```cpp
struct AudioState {
     bool has_media;
     bool is_playing;
     bool is_paused;
     uint32_t duration_sec;
     uint32_t position_sec;
     uint8_t volume;  // 0~100
     bool mute;
};
```

状态更新时机建议：
1. `SetAVTransportURI`：`has_media=true`，`position_sec=0`。
2. `Play`：`is_playing=true`，`is_paused=false`。
3. `Pause`：`is_playing=false`，`is_paused=true`。
4. `Stop`：`is_playing=false`，`is_paused=false`，`position_sec=0`。
5. 解码线程周期更新 `position_sec`。

然后 SOAP 查询统一读取该结构，避免出现“动作成功但状态不变”的矛盾。

### 6. 你当前日志中最容易踩的坑（务必处理）

你打印出来的 `CurrentURI` 包含 `&amp;`。若直接拿它做 HTTP 下载，很多服务端会返回错误或鉴权失败。

建议在播放线程拉流前做一次 XML 实体反转义：
1. `&amp;` -> `&`
2. `&lt;` -> `<`
3. `&gt;` -> `>`
4. `&quot;` -> `"`
5. `&apos;` -> `'`

其中 `&amp;` 是首要项，优先先做这一条也能显著提高可播放率。

### 7. 首版验收标准（minimp3 接入完成的定义）

1. 可稳定播放一条 MP3 直链超过 60 秒。
2. `Play/Pause/Stop` 能影响实际音频输出。
3. `GetTransportInfo` 与 `GetPositionInfo` 返回值和真实播放状态一致。
4. 手机端在播放中不自动回退到本机。

达到以上 4 条，即可认为“DLNA 控制面 + minimp3 数据面”首版闭环完成。

---

## 十、 关于音频采样率 (Sample Rate) 与硬件匹配的核心问题

针对“怎么知道需要多少频率的音乐”以及“能否指定”的问题，在底层嵌入式播放器中是非常核心的一环。解答如下：

### 1. 它是怎么知道这首歌是多少频率的？
MP3 文件（或网络二进制流）的每一个“帧头（Frame Header）”里，都自带了该帧的采样率（例如 44.1kHz、48kHz 等）和声道数信息。
当我们将数据喂给 `minimp3` 解码时，解码器会自动解析出这个数据并反馈给我们：

```cpp
mp3dec_frame_info_t info;
int bytes_decoded = mp3dec_decode_frame(&mp3d, buffer, bytes_left, pcm_buf, &info);

// 只要解码成功，即可直接获取真实频率和声道：
// info.hz 就是这首歌发声所必须的频率（例如 44100 意味着 44.1kHz）
// info.channels 是声道数（例如 2 意味着立体声）
```
因此，**你不需要提前去猜，也不需要去向网络请求频率**，只要解开第一帧数据就全知道了。

### 2. 这个频率可以指定（强制改变）吗？
**结论是：MP3解码器不能凭空“强行”改变歌曲本身的采样率。**
一首源文件是 44.1kHz 的音乐，解码后输出的 PCM 数组就是针对 44.1kHz 时钟生成的。如果你的音响硬件（I2S 接口）由于某种原因被“硬编码”定死在 48kHz 输出，你直接把这首 44.1kHz 歌曲喂给音响，它不会报错，而是会导致**声音变调（像开了 1.1 倍速一样的尖锐甚至破音）**。

在 WS63 等 MCU 对接平台设备时，处理这个匹配问题的途径主要有两个：

#### ★ 最佳方案：让 I2S 硬件动态适应 MP3 (强烈推荐)
这是智能音响最标准的做法。不要将 I2S 硬件初始化死在一个固定的频率。我们需要在播放线程中判断：当第一次解出 MP3 帧或者发现 `info.hz` 变化时，立刻调用底层驱动去重新配置 I2S 时钟。

```cpp
// 伪代码示例：自动动态配置
static int current_hw_hz = 0;

if (info.hz > 0 && info.hz != current_hw_hz) {
    osal_printk("检测到新歌曲采样率: %d Hz (原: %d Hz)，正在切换硬件...\n", info.hz, current_hw_hz);
    
    // 1. 禁用或清空当前的 I2S DMA 以防爆音
    i2s_disable();
    
    // 2. 调用 WS63 的底层驱动重新下发参数
    // 将 I2S 的输入时钟修改为刚刚解析出的 info.hz 
    i2s_set_sample_rate(info.hz);       // <- 这里填入你底层的驱动接口
    i2s_set_channel(info.channels);     // （如果支持，把单/双声道也切了）
    
    // 3. 重新启用 I2S
    i2s_enable();
    
    current_hw_hz = info.hz;
}

// 接下来放心大胆往 I2S 的硬件 Buffer 里推入数据就行了
i2s_write(pcm_buf, info.samples * info.channels * sizeof(short));
```

#### 备选方案：软件重采样 (Software Resample)
如果你的外置 Codec / 功放芯片非常死板，被焊死了只能以一种频率（比如 48kHz）工作，那唯一的方式是在喂给硬件前，写一个软件算法把 44.1kHz 的 PCM 数组通过插值强行“变”成 48kHz。
*   **劣势：** 在 WS63 这种只有不到 200Mhz 的轻量级 MCU 上，执行插值音频重采样（SRC）会消耗大量的 CPU 运算能力和 RAM，极易导致播放卡顿断流。**一般绝对不推荐在 MCU 上搞这种软件插值运算。**

**总结：** 放心使用 `minimp3` 提取 `info.hz` ，然后利用 WS63 较灵活的外设时钟树，做到“歌是多少赫兹，硬件就自动匹配成多少赫兹”。目前的主流音乐平台通过 DLNA 推送的 URL ，通常 95% 以上都是标准的 `44100Hz`，极少部分超清是 `48000Hz`。动态重配 I2S 能极其完美地涵盖这些需求。