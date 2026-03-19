# 海思 WS63 简易 DLNA 音箱实现指南 (新手向)

## 0. 核心思路：避繁就简
实现一个完整的 DLNA/UPnP 协议栈非常复杂，但如果我们的目标只是**让手机能搜到音箱**并**推送音频**，我们可以采用“欺骗”策略：
1.  **设备发现 (SSDP)**：手动发几个 UDP 包，让手机觉得“这里有个 DLNA 音箱”。
2.  **能力伪装 (XML)**：告诉手机“我只支持 WAV/LPCM 格式”，迫使手机 App (如 BubbleUPnP) 帮我们把 MP3 解码成原始 PCM 数据。
3.  **音频接收 (TCP)**：像通过 Socket 接收文件一样接收音频流，直接丢给 I2S 播放。

---

## 第一阶段：让世界发现你 (SSDP 实现)

SSDP (简单服务发现协议) 是 UPnP 的基础。设备加入网络后，通过 UDP 组播告诉大家“我来了”。

### 1.1 基础知识
*   **组播地址**: `239.255.255.250`
*   **端口**: `1900`
*   **交互方式**: 
    1.  **监听**: 手机发 `M-SEARCH` 搜索包，我们收到后回复 `HTTP/1.1 200 OK`。
    2.  **广播**: 我们定时发 `NOTIFY` 包，主动刷存在感。

### 1.2 核心代码实现 (参考)

在 WS63 上，你需要创建一个独立的 Task 来运行这个逻辑。

```c
#include "lwip/sockets.h"
#include "lwip/igmp.h"

#define SSDP_MCAST_ADDR "239.255.255.250"
#define SSDP_PORT 1900

// 你的设备描述文件地址，稍后会用到
#define LOCATION_URL "http://YOUR_DEVICE_IP:49152/description.xml"
// 设备的唯一标识符 (UUID)，可以用 MAC 地址生成
#define DEVICE_UUID "uuid:5f9ec1b3-ed59-79ba-4530-YOUR_MAC_ADDR"

// 响应 M-SEARCH 的报文模板
static const char *ssdp_response_template =
    "HTTP/1.1 200 OK\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "EXT:\r\n"
    "LOCATION: %s\r\n" // 填入 LOCATION_URL
    "SERVER: HiSpark-WS63/1.0 DLNA/1.0 UPnP/1.0\r\n"
    "ST: upnp:rootdevice\r\n" // 服务类型
    "USN: %s::upnp:rootdevice\r\n" // 填入 DEVICE_UUID
    "\r\n";

void dlna_ssdp_task(void *arg) {
    int sock = -1;
    struct sockaddr_in local_addr;
    struct ip_mreq mreq;
    char rx_buffer[1024];

    // 1. 创建 UDP Socket
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        printf("SSDP: Failed to create socket\n");
        return;
    }

    // 2. 允许多个应用复用端口 (SO_REUSEADDR)
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // 3. 绑定本地端口 1900
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(SSDP_PORT);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        printf("SSDP: Bind failed\n");
        close(sock);
        return;
    }

    // 4. 加入组播组 (最关键的一步！)
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_MCAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY); // 使用默认网络接口
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) < 0) {
        printf("SSDP: Failed to join multicast group\n");
        close(sock);
        return;
    }

    printf("SSDP: Started listening on port 1900...\n");

    while (1) {
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);
        
        // 5. 接收数据
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, 
                           (struct sockaddr *)&sender_addr, &sender_len);
        
        if (len > 0) {
            rx_buffer[len] = 0;
            // 6. 简单的字符串匹配：如果包含 "M-SEARCH" 和 "upnp:rootdevice"
            if (strstr(rx_buffer, "M-SEARCH") && strstr(rx_buffer, "upnp:rootdevice")) {
                printf("SSDP: Received search request from %s\n", inet_ntoa(sender_addr.sin_addr));
                
                // 7. 构建回复包
                char response[512];
                snprintf(response, sizeof(response), ssdp_response_template, LOCATION_URL, DEVICE_UUID);
                
                // 8. 单播回复给发送者
                sendto(sock, response, strlen(response), 0, 
                       (struct sockaddr *)&sender_addr, sender_len);
            }
        }
    }
}
```

---

## 第二阶段：告诉别人你是谁 (设备描述)

当手机收到上面的 UDP 回复后，它会立刻发起一个 TCP 连接（HTTP GET），访问 `LOCATION` 字段里的 URL (`http://IP:49152/description.xml`)。我们需要在此处提供一个 XML 文件。

### 2.1 极简 XML 模板
这个 XML 是 DLNA 的身份证。重点是 `<mimetype>audio/L16</mimetype>` (PCM) 和 `<friendlyName>` (音箱名字)。

```xml
<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion>
    <major>1</major>
    <minor>0</minor>
  </specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
    <friendlyName>HiSpark WS63 Speaker</friendlyName>
    <manufacturer>Me</manufacturer>
    <modelName>WS63 DIY</modelName>
    <UDN>uuid:5f9ec1b3-ed59-79ba-4530-YOUR_MAC_ADDR</UDN>
    <serviceList>
      <!-- 这里通常需要定义 AVTransport 和 ConnectionManager 服务 -->
      <!-- 为了极简，初期可以先留空或者照抄一个最小集，手机有时只看 Device 类型 -->
    </serviceList>
  </device>
</root>
```

### 2.2 简单的 HTTP Server 实现
你需要再开一个 Task，创建一个 TCP Socket 监听 49152 端口。

```c
void dlna_http_task(void *arg) {
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    // ... bind 到 49152 端口, listen ...

    while (1) {
        int conn_sock = accept(listen_sock, ...);
        if (conn_sock >= 0) {
            // 读取请求
            recv(conn_sock, buf, ...);
            
            // 如果请求包含 "GET /description.xml"
            if (strstr(buf, "GET /description.xml")) {
                // 发送 HTTP 头
                char *header = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\n\r\n";
                send(conn_sock, header, strlen(header), 0);
                // 发送 XML 内容
                send(conn_sock, YOUR_XML_STRING, strlen(YOUR_XML_STRING), 0);
            }
            close(conn_sock);
        }
    }
}
```

---

## 第三阶段：接收音频 (透传播放)

这是最关键的一步。在 DLNA 标准中，播放动作是通过 SOAP (HTTP POST XML) 控制的 `SetAVTransportURI` 和 `Play` 命令触发的。

但在**极简模式**下，如果通过特定 App (如 BubbleUPnP) 的 "Audio Cast" 功能，流程会简化：
1.  手机 App 可能会直接向你发送一个 HTTP PUT/POST 请求，或者让你去 GET 一个流地址。
2.  如果不实现复杂的 SOAP 控制，可以尝试以下捷径：
    *   在 XML 的 `<serviceList>` 中声明支持 `AVTransport`。
    *   当收到 `SetAVTransportURI` 时，解析出其中的 `CurrentURI` (通常是 http://手机IP:端口/stream.wav)。
    *   WS63 作为 HTTP Client，去连接这个 URL。
    *   **接收到的数据流直接就是 PCM 音频数据**。
    *   将收到的 buffer 直接喂给你的 `i2s_write` 接口。

### 关于“手机解码”的设置
为了让手机发给你 PCM 而不是 MP3：
1.  使用 **BubbleUPnP** (Android) 或类似强大 App。
2.  进入设置 -> Chromecast/DLNA 渲染器设置。
3.  找到你的设备，设置 **"FFmpeg 解码"** 为 **"Always Decode (总是解码)"**。
4.  设置目标格式为 **"WAV"** 或 **"LPCM"**。

---

## 调试建议

1.  **PC 端工具**: 下载 **"Developer Tools for UPnP Technologies"** (Intel 出品，现开源) 或者 **Device Spy**。
    *   运行它，看能不能在列表里刷出 "HiSpark WS63 Speaker"。
    *   如果刷不出，检查 SSDP 代码和防火墙。
2.  **抓包神器**: Wireshark。
    *   过滤 `udp.port == 1900`。
    *   看有没有来自你板子 IP 的数据包。
3.  **日志**: 在 WS63 上多打 `printf`，尤其是收到网络包的时候。

## 总结
不要试图去移植那种几万行的 `libupnp` 库。就用手写的 Socket 实现：
1.  **UDP 响应搜索** (我是设备！)
2.  **TCP 提供描述** (我是音箱！)
3.  **TCP 下载/接收流** (给我数据！)

祝你成功点亮声音！
