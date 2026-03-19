# 基于海思WS63与微信小程序的无线音响网络连接开发指南

作为一名刚接触嵌入式网络的新人，使用海思星闪 WS63 芯片实现“通过微信小程序控制音响播放音乐”，是一个绝佳的入门实战项目。

本文档将结合[官方软件开发指南](https://docs.hisilicon.com/repos/fbb_ws63/zh-CN/master/software/%E8%BD%AF%E4%BB%B6%E5%BC%80%E5%8F%91%E6%8C%87%E5%8D%97/%E8%BD%AF%E4%BB%B6%E5%BC%80%E5%8F%91%E6%8C%87%E5%8D%97.html#id8)与 SDK 库文件结构，为你提供完整的方案选型、海思网络库功能解析，并详细整理出逐步落地的开发步骤。

---

## 一、方案分析与选型

微信小程序控制嵌入式设备，通常有几种思路。考虑到你是要**播放音乐**，我们要重点考虑带宽与传输方式。

### 微信小程序支持的网络能力：
1. **网络请求 (Wi-Fi/蜂窝)：** HTTP/HTTPS, WebSocket, TCP/UDP 局域网通信。
2. **低功耗蓝牙 (BLE)：** 仅能传输少量指令数据，**不支持**直接传输高质量的流媒体音频（如音乐）。

### 你的可选方案：
- ? **直接使用 BLE/SLE (星闪) 传音频：** 微信小程序不支持经典蓝牙大带宽音频 Profile传输流媒体，也不支持原生的星闪高带宽传输业务。
- ? **方案A（推荐，经典智能音响模式：Wi-Fi + 云端/HTTP下载）：**
  手机通过蓝牙（或 Wi-Fi SoftAP）把家里路由器的账号密码发给 WS63音响（这叫**配网**）。音响连上互联网后，小程序只发控制指令（如：播放某首歌曲的 URL 链接）。音响板端通过 HTTP 下载/缓冲该 URL 的音频流并解码播放。
- ? **方案B（最简局域网控制：Wi-Fi +局域网 TCP/UDP）：**
  音响和手机连接同一个 Wi-Fi（路由器），或者音响开启热点（SoftAP），手机连上音响热点。音响上运行一个 TCP Server。微信小程序通过 `wx.createTCPSocket` 或局域网通信直接连接音响的 IP 发送控制指令。

> **结论：为了达到最好的扩展性，我们采用 方案A 或 方案B 的核心网络部分——使用基于 Wi-Fi 的 LWIP Socket 方案。即让 WS63 通过 Wi-Fi 连网，并建立 TCP 通信接收指令。**

---

## 二、海思 WS63 网络库功能解析

通过查询您的 SDK 目录 `fbb_ws63/src/open_source` 和 `fbb_ws63/src/application`，海思为 WS63 提供了非常完备的网络栈：

1. **底层 Wi-Fi 控制 (`components/wifi` 或驱动库)：**
   - **STA 功能：** 作为客户端连接到家里的路由器，获取 IP 地址。
   - **SoftAP 功能：** 作为热点，让手机直接连接。
   - 共存模式：既可以发射热点，又可以连别人，非常适合初期“配网”。
2. **TCP/IP 协议栈 (`open_source/lwip/`)**
   - 提供了工业级标准的套接字（Socket）编程接口。你可以基于它使用 `socket()`, `bind()`, `listen()`, `accept()` 等标准 C 语言网络函数，来创建 TCP 服务器或客户端。
3. **物联网应用层协议 (`open_source/mqtt/`, `open_source/libcoap/`)**
   - 如果你想后续把它做成能远程控制的物联网音响（不在家里也能控制点歌），可以使用 MQTT 协议对接阿里云、腾讯云等。
4. **数据解析 (`open_source/cjson/`)**
   - 微信小程序通常发下来的是 JSON 格式的字符串（例如 `{"cmd":"play", "url":"http://xxx.mp3"}`）。你可以用开源的 cJSON 库来解析这些指令。

---

## 三、开发落地步骤详解

以下步骤指导如何建立“微信小程序”和“WS63音响”之间的网络联系。

### 步骤 1：让音响连接到局域网 (Wi-Fi STA功能)

设备必须先拿到 IP 地址，才能进行 TCP 通信。可以参考官方文档：**Wi-Fi 软件开发 -> STA功能**。

**关键API链路：**
```c
// 伪代码参考，具体请查看 application/samples/wifi/sta_sample
#include "wifi_api.h"
#include "netifapi.h"

// 1. 初始化 Wi-Fi 驱动
wifi_init();

// 2. 使能 STA
wifi_sta_enable();

// 3. 连接指定的 Wi-Fi (硬编码调试用，后续可改为蓝牙写入)
wifi_sta_config_advance_t config = {0};
// 填入SSID和密码 ...
wifi_sta_connect(&config); 

// 4. 启动 DHCP 客服端获取 IP
netifapi_dhcp_start(netif);
```
? **参考重点:** 官方示例路径 `application/samples/wifi/sta_sample`。通过执行 `python3 build.py ws63-liteos-app menuconfig` 并使能 `Support WIFI STA Sample`，可以直接编译烧录体验。

### 步骤 2：在音响上建立网络服务端 (TCP Server)

WS63 拿到 IP 之后，我们需要开启一个服务端口，等待微信小程序连接。你需要使用 LWIP Socket。

```c
#include "lwip/sockets.h"

void tcp_server_task(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0); // 创建 TCP Socket
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8888); // 监听 8888 端口
    
    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_fd, 5);
    
    while (1) {
        // 阻塞等待手机（微信小程序）连入
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd >= 0) {
            char rx_buf[256];
            int len = recv(client_fd, rx_buf, sizeof(rx_buf) - 1, 0); // 接收小程序指令
            if (len > 0) {
                rx_buf[len] = '\0';
                printf("Received from WeChat: %s\n", rx_buf);
                
                // 【调用音频解码播放模块】
                // handle_music_cmd(rx_buf);
                
                // 返回确认信息给小程序
                send(client_fd, "OK", 2, 0);
            }
            close(client_fd);
        }
    }
}
```

### 步骤 3：微信小程序端发指令

打开微信开发者工具，在 `pages/index/index.js` 中使用原生的 TCP API 去连接你在**步骤 1** 中获取到的音响 IP 地址（假设是 `192.168.1.100`）。

```javascript
// 小程序端代码示例
const tcp = wx.createTCPSocket()
tcp.connect({
  address: '192.168.1.100', // 这里填音响控制台打印出来的IP
  port: 8888 // 对应步骤2中监听的端口
})

tcp.onConnect(() => {
  console.log('成功连接到音响!')
  // 建立连接后，发送控制指令 (JSON格式)
  const cmd = JSON.stringify({
    action: "play",
    volume: 80,
    url: "http://music.domain.com/song.mp3" // 想要播放的歌曲的网络链接
  })
  tcp.write(cmd)
})

tcp.onMessage((res) => {
  // 接收音响的回复
  console.log('音响回复：', new Uint8Array(res.message))
})
```

---

## 四、高可扩展性系统架构设计建议

作为新手，写出来的代码往往容易揉成一团。建议在设计你的工程结构时，遵循以下**解耦（分层）思路**：

1. **硬件与驱动层 (Driver)：** 封装 Wi-Fi 的连接、断开等，封装一个 `Network_Init()` 接口。
2. **通信层 (Communication)：** 维护 TCP Server 或 MQTT Client 任务。收到数据后，不要在通信任务里直接播放音乐，而是将数据放入一个 **消息队列 (Message Queue)** 或者触发一个回调函数。
3. **协议拆解层 (Protocol)：** 使用 SDK 自带的 `cJSON` 库，从通信层拿到的纯文本中取出 `"action": "play"`。
4. **业务应用层 (Application)：** 根据拆解出的命令，去调用底层的音频组件（I2S / Codec 控制）。比如传入 url 获取流并解码输出到喇叭。此时若引入带操作系统的应用，推荐使用 LiteOS 提供的多任务调度机制 (Task / Thread)。

## 五、新人实操建议避坑指南

1. **先独立调通网络：** 不要一开始就把网络和音频控制绑在一起写。先跑通 `sta_sample`，确保能在路由器后台或者串口工具里看到你的开发板拿到了 IP。
2. **善用网络调试助手：** 写完了 TCP Server 代码跑在板子上后，先别急着用小程序写 UI。用电脑上的“网络调试助手（TCP Client）”连接板子发测试指令，验证收发情况！
3. **看门狗（Watchdog）：** 就像官方文档最后部分所写，如果遇到打流或业务挂起时频繁自动重启，那多半是卡死触发了看门狗。要注意定期“喂狗”（`uapi_watchdog_kick()`），或者调整你的线程优先级避免阻塞系统线程。
4. **深入学习海思示例结构：** 所有的业务开发，建议先在 `application/samples/` 复制一个工程出来依葫芦画瓢修改 `CMakeLists.txt`。

---

## 六、进阶：实现“手机全局音频”与“特定App独立推流”双模共存

你提出的需求极其符合现代高端智能音响（如HomePod、小度音箱）的标准产品定义！你希望将音响做成兼容两种场景下的产品：
1. **全局音频模式**：把音响当普通无线音响，不管手机是在刷短视频、打游戏还是发语音，所有的“局内声音”都从音响直接输出。
2. **独立推流模式 (仅接管特定App)**：音响自己利用网络放音乐，此时你的手机干别的（比如接电话、刷抖音，系统音效仍然从手机出），完全不影响音响播放QQ音乐。

我前文为你提供的**“方案A (Wi-Fi + TCP指令 -> 音响自行拉取流媒体)”，完美支持了第2种需求**，但这还不足以捕获所有的手机全局声音。为了实现这种“双模切换”，你需要结合海思的 Wi-Fi 与 蓝牙/星闪(SLE) 软硬体资源，在同一套代码内实现跨应用融合：

### 模式一：直接播放手机全局声音（经典蓝牙 A2DP 或 星闪 SLE 音频）
- **实现原理：** 这种模式下音响仅仅是一个接收终端（Audio Sink），手机负责解码和混音，把音频压缩后通过射频发给外设。
- **WS63侧开发能力：** 根据海思文档，WS63可以作蓝牙和Wi-Fi共存。要实现全局音频，需要在版端开启或移植 **经典蓝牙音频协议 (A2DP Sink)** 或针对支持星闪的新款手机开放 **SLE高宽带低延迟音频** 通道。
- **数据流向：** 手机全局音频 -> 手机本身编解码 -> 蓝牙/星闪射频 -> WS63蓝牙协议栈 -> I2S输出 -> 喇叭。

### 模式二：仅接管特定音乐软件（Wi-Fi DLNA / 自建云端投播）
- **实现原理：** QQ音乐、网易云音乐等软件具备“投屏（DLNA / QPlay / AirPlay）”功能。此时手机本质上变成了一个**网络遥控器**。它截获了歌曲并且仅仅是把歌曲所在的 URL 网络链接，通过 Wi-Fi 指令发给了音响。
- **自定义方案（直接用小程序）：** 如果你想通过自己的**微信小程序**做，就是完全采用前文第二部分的方案。小程序作为你的“私人版QQ音乐控制台”，将 URL 发给 TCP Socket，WS63的 Wi-Fi 自己去云端拉流。
- **原生App支持方案（DLNA移植）：** 为了让 QQ音乐/网易云 的自带“投射 TV 设备”列表里能原生态搜到你的音响，你需要在 WS63 版端基于内部已有的 LwIP 网络栈，移植一套开源的跨平台 **UPnP/DLNA 服务端框架**（如 `libupnp` 或 `Platinum`）。音响运行起被称之为 “DLNA Renderer” 服务时，QQ 音乐会自动在同一局域网下搜索到它。

### 系统如何设计，让程序支持这套双模方案？
由于这两种模式占用的资源（网络套接字 vs 射频蓝牙音频缓冲区）不同，你在搭建软件时，需用基于状态机的设计。此时，你开发的微信小程序就是这个音响的高级**中央配网与模式切换遥控器**：

1. **用户在小程序点击【开启蓝牙全局模式】** 
   - 微信小程序通过 TCP Socket 发送 JSON：`{"action": "switch_mode", "mode": "bluetooth"}`。
   - WS63 板端收到指令 -> 挂起 Wi-Fi HTTP 音乐下载任务 -> 激活底层 A2DP/SLE Audio 广播功能，等待手机进行蓝牙配对发音。
2. **用户在小程序点击【进入Wi-Fi独立推流】** 
   - 发送 JSON：`{"action": "switch_mode", "mode": "wifi_dlna"}`。
   - WS63 端断开手机当前的射频音频链接连接，防止短视频声音意外切入 -> 核心切换回 Wi-Fi 下载或 DLNA 监听任务 -> 从服务器或者其他App接收 URL 并播放音乐。

**结论**：你可以放心干！这套组合硬件基座完全能够完美胜任你的想法。依靠海思系统自带的 `LiteOS` 实时操作系统支持，配合双模无线设计，你完全可以将 **网络套接字监听任务** 和 **蓝牙广播/回传任务** 设计成多个 `Thread`。通过事件标志位互相切换，这正是当今所有高级智能音箱底层做的事情！

---

## 七、海思官方 `sta_sample.c` 源码级流程拆解

前文提到了一定要先跑通官方的 `sta_sample`，这是整个音响能够连入局域网的“地基”。为了让你这位嵌入式网络新人能完全看懂底层的 C 代码，我们以 `application/samples/wifi/sta_sample/sta_sample.c` 为例，为你做白话级的代码流程深度解析。

这个示例的核心思想是：**基于 LiteOS 系统的独立线程，使用“状态机”设计模式，异步完成 Wi-Fi 扫描、配对、获取 IP 的全流程。**

### 第一步：入口与线程创建 (OS 层)
在文件最底部，你会看到这样两段代码：
```c
/* 运行入口宏 */
app_run(sta_sample_entry);

static void sta_sample_entry(void) {
    // ... 配置线程属性 (名称、优先级、栈大小)
    osThreadNew((osThreadFunc_t)sta_sample_init, NULL, &attr); // 创建并启动线程
}
```
* **原理解析**：单片机不再是从头到尾 `while(1)` 跑到底，而是引入了 LiteOS 操作系统。`app_run` 会在系统启动后自动调用 `sta_sample_entry`。这个函数创建了一个名为 `sta_sample_task` 的多任务线程。以后你的网络业务就在这个独立线程里跑，不会卡死主程序。

### 第二步：参数初始化与回调注册 (Init 层)
线程启动后，会进入 `sta_sample_init` 函数：
```c
/* 1. 注册事件回调 */
wifi_register_event_cb(&wifi_event_cb);

/* 2. 等待底层物理驱动就绪 */
while (wifi_is_wifi_inited() == 0) {
    osDelay(10); 
}

/* 3. 进入主逻辑 */
example_sta_function();
```
* **什么是回调 (`Callback`)？**：Wi-Fi 扫描周围环境或者连接路由器，是需要花好几秒时间的。CPU不能傻等着（这叫阻塞）。这里的做法是，告诉底层驱动：“我交给你两个函数 (`wifi_scan_state_changed`, `wifi_connection_changed`)，你扫描完了或者连上网了，就来调用这俩函数通知我。”

### 第三步：核心状态机流转 (业务层)
`example_sta_function()` 是核心灵魂。它通过一个 `g_wifi_state` 全局变量记录当前进行到了哪一步。整个 `while(1)` 轮询就像流水线一样：

1. **状态 0: `INIT` (初始化/启动扫描)**
   * **动作**：调用 `example_sta_scan()` 告诉网卡去扫描名字叫 `"my_softAP"` 的网络。
   * **切换**：状态变为 `SCANING`（扫描中）。
2. **状态 1: `SCANING` (扫描中等待)**
   * **动作**：啥也不干，只是等底层事件。当底层扫描完毕，会触发前面注册的回调函数 `wifi_scan_state_changed`，在这个回调里偷偷把状态修改为 `SCAN_DONE`。
3. **状态 2: `SCAN_DONE` (扫描完成，处理结果)**
   * **动作**：调用 `example_get_match_network()` 在扫描到的一大堆列表里，找出名字叫 `"my_softAP"` 的那个，并把密码 `"my_password"` 打包进连接参数里。
   * **切换**：找到了就进入 `FOUND_TARGET`，没找到就退回 `INIT` 重新扫。
4. **状态 3: `FOUND_TARGET` ＆ `CONNECTING` (发起连接)**
   * **动作**：调用了 `wifi_sta_connect(&expected_bss)` 真正向路由器发起握手。此时状态切为 `CONNECTING`。
   * 等待刚才注册的第二个回调 `wifi_connection_changed` 被触发，如果连接成功，它会把状态修改为 `CONNECT_DONE`。
5. **状态 4: `CONNECT_DONE` (连上路由器，开始要 IP)**
   * **动作**：连上路由器不代表能上网，还必须要有 IP 地址。这步调用 `netifapi_dhcp_start` 呼叫路由器给你分配一个 IP 网址。状态变为 `GET_IP`。
6. **状态 5: `GET_IP` (检查 DHCP 获取结果)**
   * **动作**：每隔一会儿读取一次看拿到 IP 没有 (`example_check_dhcp_status`)。只要拿到了，整个死循环 `break`，流程宣告大功告成！这个时候你就可以在另外的地方去建刚才提到的 `TCP Socket Server` 了。

---

### 给新手的修改建议（把它变成你的配网代码）：
理解了这个流转以后，你就可以开始魔改了。
你可以修改：`example_get_match_network` 函数。把里面硬编码的 `#define WIFI_SCAN_EXPECT_SSID "my_softAP"` 以及密码 `"my_password"`，替换成你 **后期通过热点或者蓝牙（也就是我们在前面沟通的配网步骤）接收到的真实家用的账号和密码变量**。
这样一来，你的音响配网基石就彻底写好了！

---

## 八、新手实战：从零开始的代码编写顺序

面对一个全新的软硬件工程，新手最忌讳的就是“一上来就把所有功能写在一起”。请严格按照以下**“解耦递进”**的开发顺序来写代码，每完成一步，都要编译跑到板子上验证一遍：

### ? 第一阶段：跑通骨架（点亮系统与多任务）
**目标**：不碰网络，只建骨架。
1. **创建主任务**：写一个类似于 `main_task_entry` 的函数，使用 `osThreadNew` 把它挂载到 LiteOS 上。
2. **循环打印**：在任务里写一个 `while(1)`，里面只放一个 `osDelay(1000)` 和 `printf("System Running...\r\n")`。
3. **编译烧录**：确保能在串口工具里看到你的打印，这就证明你的系统跑起来了，而且看门狗不会复位你。

### ? 第二阶段：硬编码连接家里路由器（STA网络层）
**目标**：让板子成功拿到 IP。
1. **移植 STA 代码**：将 `sta_sample.c` 里的初始化宏、回调函数、状态机逻辑抄进你的代码里。
2. **写死账号密码**：这个时候**不要写配网功能**，直接用宏定义把 `WIFI_SCAN_EXPECT_SSID` 改成你家路由器的名字，把密码写死在代码里（比如 `"12345678"`）。
3. **打印 IP**：确保 `example_check_dhcp_status` 成功后，能用 API 获取并打印出路由分配给你的局域网 IP（比如 `192.168.3.15`）。能在电脑上 `ping 192.168.3.15` 成功，说明网络通路打通！

### ? 第三阶段：搭建服务端与小程序打通（TCP通信层）
**目标**：板子能收到微信发来的文字。
1. **启动 TCP Server**：在上一阶段拿到 IP 后（退出 `while(1)` 之后），紧接着启动 `socket() -> bind() -> listen()`。
2. **挂起等待**：调用 `accept()` 让程序挂起，等待连接。
3. **微信小程序端开发**：用微信开发者工具，按照前文提供的 `wx.createTCPSocket` 写个最简陋的界面，搞个按钮。填入上一阶段拿到的 IP 发送一段文字（比如 `"Hello WS63"`）。
4. **验证接收**：板子通过 `recv()` 收到后打印出来，说明跨设备的“奇通”正式建立。

### ?? 第四阶段：数据解析与指令分发（协议层）
**目标**：能听懂微信发来的指令是什么。
1. **JSON 解析**：在板子上引入 `open_source/cjson` 库。
2. **解析协议**：当你的 TCP 收到微信发来的 `{"action":"play", "url":"http://test.mp3"}` 字符串时，用 `cJSON_Parse` 把它拆开。
3. **打印指令**：在串口打印：“获取到播放动作，歌曲链接是：xxx”。到这一步，“网络遥控器”的功能逻辑就闭环了。

### ?? 第五阶段：补齐动态配网（抹除硬编码代码）
**目标**：消除第二阶段写死的账号密码，让它是真正商品级的。
1. **改写逻辑**：修改启动流程为主任务启动后先不进入 STA 连路由器，而是先进入 **SoftAP (热点) 模式** 或 **BLE (蓝牙) 模式**。
2. **获取密码**：写代码创建一个热点并等待新的 TCP 数据（收到 SSID 和密码）。
3. **切换网络**：拿到用户传来的账号密码后，保存到变量，关闭 SoftAP 或者蓝牙，然后再进入第二阶段的代码，把变量传进去连接相应的路由器。

### ? 第六阶段：对接硬件生态（音频编解码与双模）
**目标**：真正的“音响”发声。
1. **对接 I2S**：引入底层的音频库或者编解码芯片驱动。
2. **业务对接**：在第四阶段解析出 URL 的地方，调用 HTTP 下载流并喂给音频解码器播放。
3. **开启多模（进阶）**：如第六节所说，做成独立的热点+全局蓝牙音频兼顾的系统。

**总结**：先跑通基本骨架，再连死网络，建好 TCP 服务，成功跑通一段假数据后，再去搞比较复杂的“配网交互”和“播流”。一步一个脚印，你绝对没问题的！遇到某个具体 API 不知道怎么调，可以专门开新问题问我。
