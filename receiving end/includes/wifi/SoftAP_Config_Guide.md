# SoftAP配网及后续DLNA开发指南（基于海思WS63 LwIP）

你好！作为网络编程的新手，不用担心，你的思路已经非常清晰了。这篇指南将详细解答你的疑问，并手把手教你如何在**海思WS63**芯片上基于**官方LwIP协议栈**实现SoftAP配网功能。

## 1. 核心疑问解答

### 1.1 TCP 还是 UDP (你提到的UWP应该是UDP的笔误)？
在局域网内进行简单的配网数据传输，**TCP 和 UDP 都可以**。
* **TCP (传输控制协议)**：就像打电话，需要先建立连接，数据保证不丢失，顺序也不会乱。非常可靠，但代码稍微多几行。海思官方文档也提供了完整的TCP服务端示例。
* **UDP (用户数据报协议)**：就像写信或者大喇叭广播，发出去就不管了，不需要建立连接。代码极其简单，尤其在微信小程序端很容易实现。海思官方文档有详细的UDP示例。

**推荐**：对于初学者，在SoftAP这种手机和设备直接连接的极简网络下（距离近、信号好、几乎不丢包），利用 **UDP** 来传输SSID和密码是非常合适的，代码量最小。本文将以UDP为例，因为最适合快速打通。

### 1.2 配网方式是否会影响后续的 DLNA (网络音响) 功能？
**绝对不会！请放心大胆地写。**
SoftAP配网 和 DLNA 是两个**完全独立**的阶段和功能：
1. **配网阶段**：设备开启热点（AP模式），此时设备相当于一个路由器（IP是你在代码里写的 `192.168.43.1`），用来接收手机发来的账号密码。此时设备开启DHCP服务器供手机获取IP。
2. **正常工作阶段 (DLNA)**：拿到密码后，设备关闭热点，切换为 STA 模式，连接到你家里的路由器。连上路由器后，设备才开始运行 DLNA 的相关代码。

所以，配网阶段的数据传输无论采用什么协议，用完就结束了，和未来的 DLNA 功能互不干扰。

---

## 2. 系统方案设计

你的设备（基于WS63/LwIP）和微信小程序的交互流程如下：

1. **设备端**：调用 `softap_start()` 开启热点（2026_sound）。
2. **设备端**：创建一个基于 LwIP 的 UDP Socket（监听端口，比如 `8080`），并在一个线程里循环等待接收数据。
3. **手机端**：用户手动去手机设置里连接 `2026_sound` 这个Wi-Fi。
4. **微信小程序**：用户输入家里路由器的账号密码，点击“发送”。小程序通过 UDP 将数据（可以是 JSON 格式如 `{"ssid":"xxx", "pwd":"yyy"}`）发送到 `192.168.43.1:8080`。
5. **设备端**：收到 UDP 包，解析出 SSID 和 密码。
6. **设备端**：保存信息，关闭 UDP Socket，关闭 SoftAP，然后调用 `sta_init()` 和 `sta_start()` 去连接家里的路由器。

---

## 3. 详细代码示例（基于海思LwIP官方文档）

LwIP 是嵌入式常用的轻量级网络协议栈。海思WS63集成了完整的LwIP协议栈支持。

### 3.1 关键头文件包含

```c
#include <stdio.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "lwip/sockets.h"    // lwIP socket API
#include "cJSON.h"           // 用于JSON解析（你的工程open_source/cjson/目录下有）
```

> **重要**：根据海思官方文档，在WS63上使用lwIP比标准POSIX有所不同：
> - 使用 `lwip_socket()`, `lwip_bind()`, `lwip_recvfrom()` 等lwIP专用函数
> - 或者使用线程安全的 `netifapi_*` API（用于网卡配置）
> - 关闭socket时用 `closesocket()` 而不是 `close()`

### 3.2 设备端 (C / LwIP)：创建 UDP 服务端接收数据

参考海思官方LwIP开发指南中的UDP示例代码，以下是适配你的SoftAP场景的完整实现：

```c
#include <stdio.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#define STACK_IP "192.168.2.5"
#define STACK_PORT 2277
#define PEER_PORT 3377
#define PEER_IP "192.168.2.2"
#define MSG "Hi, I am lwIP"
#define BUF_SIZE (1024 * 8)
typedef unsigned   char    u8_t;
typedef signed     int     s32_t;
u8_t g_buf[BUF_SIZE+1] = {0};
int sample_udp() {
    s32_t sfd;
    struct sockaddr_in srv_addr = {0};
    struct sockaddr_in cln_addr = {0};
    socklen_t cln_addr_len = sizeof(cln_addr);
    s32_t ret = 0, i = 0;
    /* socket creation */
    printf("going to call socket\n");
    sfd = socket(AF_INET,SOCK_DGRAM,0);
    if (sfd == -1) {
        printf("socket failed, return is %d\n", sfd);
        goto FAILURE;
    }
    printf("socket succeeded\n");
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr=inet_addr(STACK_IP);
    srv_addr.sin_port=htons(STACK_PORT);
    printf("going to call bind\n");
    ret = bind(sfd,(struct sockaddr*)&srv_addr, sizeof(srv_addr));
    if (ret != 0) {
        printf("bind failed, return is %d\n", ret);
        goto FAILURE;
    }
    printf("bind succeeded\n");
    /* socket creation */
    /* send */
    cln_addr.sin_family = AF_INET;
    cln_addr.sin_addr.s_addr=inet_addr(PEER_IP);
    cln_addr.sin_port=htons(PEER_PORT);
    printf("calling sendto...\n");
    memset(g_buf, 0, BUF_SIZE);
    strcpy(g_buf, MSG);
    ret = sendto(sfd, g_buf, strlen(MSG),
    0, (struct sockaddr *)&cln_addr,
    (socklen_t)sizeof(cln_addr));
    if (ret <= 0) {
        printf("sendto failed,return is %d\n", ret);
        goto FAILURE;
    }
    printf("sendto succeeded,return is %d\n", ret);
    /* send */
    /* recv */
    printf("going to call recvfrom\n");
    memset(g_buf, 0, BUF_SIZE);
    ret = recvfrom(sfd, g_buf, sizeof(g_buf), 0,
    (struct sockaddr *)&cln_addr, &cln_addr_len);
    if (ret <= 0) {
        printf("recvfrom failed,
        return is %d\n", ret);
        goto FAILURE;
    }
    printf("recvfrom succeeded, return is %d\n", ret);
    printf("received msg is : %s\n", g_buf);
    printf("client ip %x, port %d\n",
    cln_addr.sin_addr.s_addr,
    cln_addr.sin_port);
    /* recv */
    close(sfd);
    return 0;
    FAILURE:
    printf("failed, errno is %d\n", errno);
    close(sfd);
    return -1;
}
int main() {
    int ret;
    ret = sample_udp();
    if (ret != 0) {
        printf("Sample Test case failed\n");
        exit(0);
    }
    return 0;
}
```

```c
#define SOFTAP_CONFIG_SERVER_PORT 8080
#define SOFTAP_CONFIG_BUFFER_SIZE 256

/**
 * SoftAP配网UDP服务端
 * 监听手机发送的Wi-Fi SSID和密码，然后配置设备以STA模式连接
 */
int start_softap_config_server(void)
{
    int sfd = -1;
    struct sockaddr_in srv_addr = {0};
    struct sockaddr_in client_addr = {0};
    socklen_t client_addr_len = sizeof(client_addr);
    char rx_buffer[SOFTAP_CONFIG_BUFFER_SIZE] = {0};
    int ret = 0;
    int recv_len = 0;
    
    printf("[SoftAP Config] Starting UDP server on port %d...\n", SOFTAP_CONFIG_SERVER_PORT);
    
    // 1. 创建 UDP Socket (AF_INET = IPv4, SOCK_DGRAM = UDP)
    // 使用标准的socket API（LwIP会自动处理）
    sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) {
        printf("[SoftAP Config] Failed to create socket, error: %d\n", errno);
        return -1;
    }
    printf("[SoftAP Config] Socket created successfully: %d\n", sfd);
    
    // 2. 绑定端口
    // 监听所有网卡 (INADDR_ANY)，绑定到 SoftAP 配置端口
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = inet_addr("192.168.43.1");  // SoftAP IP
    srv_addr.sin_port = htons(SOFTAP_CONFIG_SERVER_PORT);
    
    ret = bind(sfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    if (ret != 0) {
        printf("[SoftAP Config] Failed to bind socket, error: %d\n", errno);
        closesocket(sfd);
        return -1;
    }
    printf("[SoftAP Config] Socket bound successfully on %s:%d\n", 
           "192.168.43.1", SOFTAP_CONFIG_SERVER_PORT);
    
    // 3. 循环等待接收数据（仅接收一次配网请求）
    printf("[SoftAP Config] Waiting for Wi-Fi config from mobile...\n");
    
    // recvfrom 会阻塞在这里，直到收到手机发来的数据
    recv_len = recvfrom(sfd, rx_buffer, SOFTAP_CONFIG_BUFFER_SIZE - 1, 0,
                        (struct sockaddr *)&client_addr, &client_addr_len);
    
    if (recv_len > 0) {
        rx_buffer[recv_len] = '\0';  // 确保字符串以 null 结尾
        
        char client_ip_str[20] = {0};
        inet_ntoa_r(client_addr.sin_addr, client_ip_str, 20);
        
        printf("[SoftAP Config] Received %d bytes from %s:%d\n", 
               recv_len, client_ip_str, ntohs(client_addr.sin_port));
        printf("[SoftAP Config] Payload: %s\n", rx_buffer);
        
        // 4. 解析 JSON 数据
        // 期望格式: {"ssid":"your_wifi_name", "pwd":"your_wifi_password"}
        cJSON *root = cJSON_Parse(rx_buffer);
        if (root != NULL) {
            cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
            cJSON *pwd_item = cJSON_GetObjectItem(root, "pwd");
            
            if (ssid_item && pwd_item && 
                ssid_item->valuestring && pwd_item->valuestring) {
                
                printf("[SoftAP Config] Successfully parsed:\n");
                printf("[SoftAP Config]   SSID = %s\n", ssid_item->valuestring);
                printf("[SoftAP Config]   PWD  = %s\n", pwd_item->valuestring);
                
                // 5. 将取得的数据赋值给你的 wifi 类
                // 注意：你的wifi类中有这两个方法：
                //   set_wifi_scan_expect_ssid()
                //   set_wifi_scan_expect_password()
                char ssid_buffer[33] = {0};
                char pwd_buffer[65] = {0};
                
                strncpy(ssid_buffer, ssid_item->valuestring, 32);
                strncpy(pwd_buffer, pwd_item->valuestring, 64);
                
                // 转换为你的 std::array 格式（根据你的wifi.hpp）
                std::array<char, 33> target_ssid;
                std::array<char, 65> target_pwd;
                memcpy(target_ssid.data(), ssid_buffer, 33);
                memcpy(target_pwd.data(), pwd_buffer, 65);
                
                // 调用你的wifi类方法设置SSID和密码
                wifi::set_wifi_scan_expect_ssid(target_ssid);
                wifi::set_wifi_scan_expect_password(target_pwd);
                
                printf("[SoftAP Config] Configuration stored successfully\n");
                
                cJSON_Delete(root);
                ret = 0;  // 成功标志
            } else {
                printf("[SoftAP Config] Missing SSID or PWD in JSON\n");
                cJSON_Delete(root);
                ret = -1;
            }
        } else {
            printf("[SoftAP Config] Failed to parse JSON\n");
            ret = -1;
        }
    } else if (recv_len == 0) {
        printf("[SoftAP Config] Connection closed by client\n");
        ret = -1;
    } else {
        printf("[SoftAP Config] recvfrom failed, error: %d\n", errno);
        ret = -1;
    }
    
    // 6. 关闭 Socket
    closesocket(sfd);
    printf("[SoftAP Config] Socket closed\n");
    
    // 7. 如果配网成功，关闭 SoftAP 并启动 STA 连接目标路由器
    if (ret == 0) {
        printf("[SoftAP Config] Configuration completed, switching to STA mode...\n");
        // TODO: 调用关闭SoftAP的接口
        // softap_stop();
        // 
        // TODO: 初始化并启动STA
        // wifi::sta_init();
        // wifi::sta_start();
    }
    
    return ret;
}
```

**注意**：
- 根据海思文档，UDP数据最大长度为65332字节。你的配网JSON数据远小于这个限制。
- `inet_ntoa_r()` 是LwIP提供的线程安全版本的IP地址转换函数。
- 建议在单独的OSAL任务线程中运行此函数，避免阻塞主任务。参考你工程中的 `osal_task_create()` API。

### 3.2 微信小程序端代码示例 (JavaScript)

在微信小程序中，发送 UDP 数据非常简单。

```javascript
// 在你的小程序的某个点击发送事件中：
sendWifiConfig() {
    const ssid = "你的家庭WiFi名字";
    const pwd = "你的WiFi密码";
    
    // 组装成JSON字符串
    const payload = JSON.stringify({ ssid: ssid, pwd: pwd });

    // 创建 UDP 实例
    const udp = wx.createUDPSocket();
    udp.bind(); // 绑定本地任意端口

    console.log("正在发送配网数据: ", payload);

    // 发送数据到设备的 IP (192.168.43.1) 和 端口 (8080)
    udp.send({
        address: '192.168.43.1',
        port: 8080,
        message: payload
    });

    // 发送之后可以提示用户等待设备连接或者去检查设备状态
    wx.showToast({
      title: '配置信息已发送',
      icon: 'success'
    });
    
    // 稍后关闭socket
    setTimeout(() => {
        udp.close();
    }, 2000);
}
```

## 4. 微信小程序端代码示例 (JavaScript)

在微信小程序中，发送 UDP 数据非常简单。根据最新的微信小程序API：

```javascript
// 在你的小程序的某个点击发送事件中：
sendWifiConfig() {
    // 用户输入的数据（可以来自表单）
    const ssid = this.data.ssid;      // 例如 "my_home_wifi"
    const password = this.data.password;  // 例如 "1234567890"
    
    if (!ssid || !password) {
        wx.showToast({
          title: '请输入SSID和密码',
          icon: 'error'
        });
        return;
    }
    
    // 组装成JSON字符串
    const payload = JSON.stringify({ 
        ssid: ssid, 
        pwd: password 
    });

    // 创建 UDP Socket 实例
    const udpSocket = wx.createUDPSocket();
    
    // 绑定本地任意端口
    udpSocket.bind();

    console.log("正在发送配网数据: ", payload);

    // 发送数据到设备的 IP (192.168.43.1) 和 端口 (8080)
    udpSocket.send({
        address: '192.168.43.1',    // 设备SoftAP的IP地址
        port: 8080,                  // 你设备监听的UDP端口
        message: payload,
        success: (res) => {
            console.log('数据发送成功', res);
            wx.showToast({
              title: '配置信息已发送',
              icon: 'success'
            });
        },
        fail: (err) => {
            console.log('数据发送失败', err);
            wx.showToast({
              title: '发送失败，请重试',
              icon: 'error'
            });
        }
    });
    
    // 稍后关闭socket（给设备充足时间接收）
    setTimeout(() => {
        udpSocket.close();
        console.log('UDP socket already closed');
    }, 3000);
}
```

> **小程序端注意**：
> - 用户必须先在手机设置里手动连接到 `2026_sound` 这个热点。
> - 然后打开你的小程序，填入家庭路由器的SSID和密码，点击发送。
> - 设备收到数据后会自动切换到STA模式连接你的家庭路由器。

## 5. 在海思工程中如何集成这个代码

### 5.1 创建单独的任务线程运行UDP服务器

为了不阻塞主程序，建议在 OSAL 任务中运行UDP服务器。参考你工程的OSAL API：

```c
// 在你的主初始化函数中
void my_app_init(void)
{
    // ... 其他初始化代码 ...
    
    // 启动 SoftAP 热点
    softap_start();
    
    // 启动UDP配网任务（参考你工程的OSAL任务创建API）
    // osal_task_create("softap_config", config_server_task_entry, 
    //                  NULL, 0x1000, 0, osal_task_priority);
}

// UDP服务器任务入口
void config_server_task_entry(void *arg)
{
    int ret = start_softap_config_server();
    if (ret == 0) {
        printf("Configuration successful!\n");
        // 现在切换到STA模式
        // softap_stop();
        // wifi::sta_init();
        // wifi::sta_start();
    } else {
        printf("Configuration failed!\n");
    }
    
    // 任务退出
    osal_task_delete(NULL);
}
```

### 5.2 关于 CMakeLists.txt 配置

确保你的工程依赖了以下组件（检查你的 `CMakeLists.txt`）：

```cmake
# 你的应用 CMakeLists.txt 应该包含：

# lwIP 开发包
target_link_libraries(your_app PUBLIC lwip)

# cJSON 库（用于JSON解析）
target_link_libraries(your_app PUBLIC cjson)

# OSAL 层
target_link_libraries(your_app PUBLIC osal)

# Wi-Fi 相关
target_link_libraries(your_app PUBLIC wifi)
```

### 5.3 重要的配置宏设置

根据海思LwIP官方文档，你可能需要在 `lwipopts.h` 或相关配置文件中确认以下宏的设置：

```c
// UDP 连接数（考虑DHCP客户端也需要一个UDP连接）
#define MEMP_NUM_UDP_PCB        8

// 最大socket数量
#define LWIP_NUM_SOCKETS_MAX    9

// Socket总数
#define DEFAULT_LWIP_NUM_SOCKETS 9

// UDP接收缓冲大小（确保足够容纳配网JSON数据）
#define UDP_TTL                 255
```

## 6. 完整的工作流程

1. **设备启动阶段**：
   ```
   ① 设备启动时调用 softap_start()，开启热点 "2026_sound"
   ② 启动UDP服务器任务，监听 192.168.43.1:8080
   ③ 同时启动DHCP服务器供手机办理IP地址
   ```

2. **用户配网阶段**：
   ```
   ① 用户在手机设置中连接 "2026_sound" Wi-Fi（密码 "20260320"）
   ② 手机自动从设备的DHCP服务器获得IP（如 192.168.43.100）
   ③ 打开微信小程序，输入家庭路由器SSID和密码
   ④ 小程序发送UDP包到 192.168.43.1:8080
   ⑤ 设备收到并解析数据，保存目标SSID和密码
   ```

3. **连接正式网络阶段**：
   ```
   ① 设备关闭SoftAP热点
   ② 设备切换到STA模式
   ③ 设备使用保存的SSID和密码连接家庭路由器
   ④ 获得路由器分配的IP地址（如通过DHCP客户端）
   ⑤ 现在设备和手机在同一局域网，可以启动DLNA音响功能
   ```

## 7. 总结与下一步行动

**放心做！** 配网阶段的通信完全不会影响后续 DLNA 的开发。

**你应该按照以下步骤逐步推进**：

1. **第一步**：确认你的 `softap_start()` 已经能成功把热点开启。用手机连接上 `2026_sound` 后检查是否能获得 IP（192.168.43.x）。

2. **第二步**：参考上面的UDP接收代码，集成到你的工程里。根据你的工程结构，可能需要：
   - 在你的wifi.cpp中添加 `start_softap_config_server()` 函数
   - 或者在一个单独的文件中实现，然后从main中调用

3. **第三步**：在单独的OSAL任务中运行UDP服务器，避免阻塞主程序。

4. **第四步**：写一个极其简陋的微信小程序 demo（可以用官方的小程序开发者工具快速开发），连上热点后点按钮发送测试数据。

5. **第五步**：在串口监视器中观看设备是否正常收到并解析了数据。你应该看到类似这样的输出：
   ```
   [SoftAP Config] Socket created successfully: 3
   [SoftAP Config] Socket bound successfully on 192.168.43.1:8080
   [SoftAP Config] Waiting for Wi-Fi config from mobile...
   [SoftAP Config] Received 45 bytes from 192.168.43.100:12345
   [SoftAP Config] Payload: {"ssid":"my_home_router","pwd":"password123"}
   [SoftAP Config] Successfully parsed:
   [SoftAP Config]   SSID = my_home_router
   [SoftAP Config]   PWD  = password123
   [SoftAP Config] Configuration stored successfully
   ```

6. **第六步**：收到并解析数据后，调用 `wifi::set_wifi_scan_expect_ssid()` 和 `wifi::set_wifi_scan_expect_password()` 喂数据进去，然后退出AP模式，开启STA模式连接。

## 8. 可能遇到的问题

### 问题1：UDP recvfrom 一直阻塞，没有收到数据
**原因**：手机没有正确连接到设备热点，或者防火墙阻止。
**解决**：
- 确认手机已连接 "2026_sound"，能ping通 192.168.43.1
- 检查设备是否正确绑定了 192.168.43.1 这个IP地址

### 问题2：JSON解析失败
**原因**：手机发来的数据格式不对，或者cJSON库未链接。
**解决**：
- 在设备端添加打印语句输出原始字节数据，检查格式
- 确保CMakeLists.txt中已经链接了cjson库

### 问题3：编译提示找不到 lwip/sockets.h
**原因**：工程没有正确包含LwIP头文件路径。
**解决**：
- 检查你的 CMakeLists.txt，确保包含了lwIP的include目录
- 查看 `Components/lwip_sack/` 目录下是否有相关头文件

## 9. 参考文档

- **海思WS63 LwIP官方开发指南**：https://docs.hisilicon.com/repos/fbb_ws63/zh-CN/master/software/lwIP%20%E5%BC%80%E5%8F%91%E6%8C%87%E5%8D%97/

本指南中的代码示例直接来自海思官方文档中的"UDP示例代码"和"DHCP服务端示例代码"部分，已根据你的SoftAP配网场景进行了适配和扩展。
