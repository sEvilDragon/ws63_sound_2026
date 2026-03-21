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
    
    // ===== 情形1：请求设备描述 XML =====
    if (strstr(buffer, "GET /description.xml") != NULL) {
        // 手机搜索到你后，第一步会请求获取长篇 XML 描述信息
        const char* xml_response =
            "HTTP/1.1 200 OK\r\n"
            "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
            "CONTENT-LENGTH: 1250\r\n"
            "CONNECTION: close\r\n\r\n"
            "<?xml version=\"1.0\"?>"
            "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
            "<specVersion><major>1</major><minor>0</minor></specVersion>"
            "<device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
            "<friendlyName>HiSilicon-WS63 DLNA Speaker</friendlyName>"
            "<manufacturer>HiSilicon</manufacturer>"
            "<manufacturerURL>http://www.hisilicon.com/</manufacturerURL>"
            "<modelDescription>WiFi DLNA Audio Device</modelDescription>"
            "<modelName>WS63-DMR</modelName>"
            "<modelNumber>1.0</modelNumber>"
            "<UDN>uuid:12345678-1234-1234-1234-123456789abc</UDN>"
            "<serviceList>"
            "<service>"
            "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
            "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
            "<controlURL>/control</controlURL>"
            "<eventSubURL>/event</eventSubURL>"
            "<SCPDURL>/AVTransport.xml</SCPDURL>"
            "</service>"
            "</serviceList>"
            "</device>"
            "</root>";
        
        lwip_send(client_sock, (const uint8_t*)xml_response, strlen(xml_response), 0);
        osal_printk("已发送设备描述XML\n");
    }
    
    // ===== 情形2：处理SetAVTransportURI播放指令 =====
    else if (strstr(buffer, "POST") != NULL && strstr(buffer, "SetAVTransportURI") != NULL) {
        char media_url[512] = {0};
        
        // 提取CurrentURI标签中的URL
        // 格式如：<CurrentURI>http://xxx.mp3</CurrentURI>
        char* uri_start = strstr(buffer, "<CurrentURI>");
        if (uri_start != NULL) {
            uri_start += strlen("<CurrentURI>");
            char* uri_end = strstr(uri_start, "</CurrentURI>");
            if (uri_end != NULL) {
                int uri_len = uri_end - uri_start;
                if (uri_len > 0 && uri_len < sizeof(media_url) - 1) {
                    strncpy(media_url, uri_start, uri_len);
                    media_url[uri_len] = '\0';
                }
            }
        }
        
        osal_printk("收到播放指令，媒体URL: %s\n", media_url);
        
        // 发送 HTTP 200 OK SOAP 应答
        const char* soap_response =
            "HTTP/1.1 200 OK\r\n"
            "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
            "CONNECTION: close\r\n\r\n"
            "<?xml version=\"1.0\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body>"
            "<u:SetAVTransportURIResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
            "</s:Body>"
            "</s:Envelope>";
        
        lwip_send(client_sock, (const uint8_t*)soap_response, strlen(soap_response), 0);
        
        // 如果URL有效，将其传递给音频线程处理
        if (strlen(media_url) > 0) {
            // TODO: 将media_url放入队列或全局变量供音频线程使用
            // queue_push_audio_url(media_url);
            osal_printk("已触发音频解码任务\n");
        }
    }
    
    // ===== 情形3：处理Play播放控制 =====
    else if (strstr(buffer, "POST") != NULL && strstr(buffer, "Play") != NULL) {
        const char* play_response =
            "HTTP/1.1 200 OK\r\n"
            "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
            "CONNECTION: close\r\n\r\n"
            "<?xml version=\"1.0\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body>"
            "<u:PlayResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
            "</s:Body>"
            "</s:Envelope>";
        
        lwip_send(client_sock, (const uint8_t*)play_response, strlen(play_response), 0);
        osal_printk("已发送Play应答\n");
    }
    
    // ===== 情形4：处理Pause暂停控制 =====
    else if (strstr(buffer, "POST") != NULL && strstr(buffer, "Pause") != NULL) {
        const char* pause_response =
            "HTTP/1.1 200 OK\r\n"
            "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
            "CONNECTION: close\r\n\r\n"
            "<?xml version=\"1.0\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body>"
            "<u:PauseResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
            "</s:Body>"
            "</s:Envelope>";
        
        lwip_send(client_sock, (const uint8_t*)pause_response, strlen(pause_response), 0);
        osal_printk("已发送Pause应答\n");
    }
    
    // ===== 情形5：处理Stop停止控制 =====
    else if (strstr(buffer, "POST") != NULL && strstr(buffer, "Stop") != NULL) {
        const char* stop_response =
            "HTTP/1.1 200 OK\r\n"
            "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
            "CONNECTION: close\r\n\r\n"
            "<?xml version=\"1.0\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body>"
            "<u:StopResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\" />"
            "</s:Body>"
            "</s:Envelope>";
        
        lwip_send(client_sock, (const uint8_t*)stop_response, strlen(stop_response), 0);
        osal_printk("已发送Stop应答\n");
    }
    
    // ===== 默认情形：返回400错误 =====
    else {
        const char* error_response =
            "HTTP/1.1 400 Bad Request\r\n"
            "CONNECTION: close\r\n\r\n";
        
        lwip_send(client_sock, (const uint8_t*)error_response, strlen(error_response), 0);
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

## 六、 关键细节答疑

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