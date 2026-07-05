# 小程序接入音色预设和夜间模式教程

本文说明小程序如何接入本工程新增的音色预设和夜间模式。两个功能只通过小程序 HTTP 控制，不接入 TTP 触摸按键。

## 设备连接

小程序需要和接收端在同一网络内：

- STA 模式：手机和设备连接同一个路由器，使用 `GET /api/v1/status` 返回或 UDP 发现广播中的设备 IP。
- SoftAP 配网模式：手机连接设备热点后，直接访问设备 SoftAP IP 的 `8080` 端口。

所有接口都访问接收端：

```text
http://<device_ip>:8080
```

响应统一为：

```json
{
  "code": 0,
  "msg": "ok",
  "data": {}
}
```

## 状态接口

小程序进入页面、切回前台、操作成功后，都应该刷新状态：

```http
GET /api/v1/status
```

返回数据会包含新增字段：

```json
{
  "mode": 63,
  "mode_name": "SLE",
  "volume": 25,
  "bass": 0,
  "brightness": 50,
  "tone": 2,
  "tone_name": "BASS_BOOST",
  "night": false,
  "hotspot": "OFF",
  "network": "CONNECTED",
  "wifi_ssid": "router",
  "softap_ssid": "ws63_softap",
  "device_ip": "192.168.1.23"
}
```

## 音色预设

音色预设由接收端保存到 NV。设备重启后，接收端会恢复上一次的小程序选择。

建议小程序用分段选择器或单选列表展示：

| 名称 | tone 数值 | 请求字符串 |
| --- | ---: | --- |
| 原声 | 0 | `FLAT` |
| 人声 | 1 | `VOCAL` |
| 低音增强 | 2 | `BASS_BOOST` |
| 流行 | 3 | `POP` |
| 摇滚 | 4 | `ROCK` |

设置接口：

```http
POST /api/v1/tone
Content-Type: application/json

{"tone":"BASS_BOOST"}
```

也可以传数字：

```json
{"tone":2}
```

成功响应：

```json
{
  "code": 0,
  "msg": "ok",
  "data": {
    "tone": 2,
    "tone_name": "BASS_BOOST"
  }
}
```

小程序收到成功响应后，立即刷新 `GET /api/v1/status`，用返回值更新当前选中项。

## 夜间模式

夜间模式由控制端保存到 NV。小程序请求先发到接收端，再通过两板通信同步到控制端；控制端写入 NV 后，下次上电仍会恢复夜间模式状态。

夜间模式打开后，固件会：

- 限制实际播放音量上限到 30。
- 降低实际低音增强。
- 限制控制端灯效亮度上限到 15。
- 保留用户原来的 `volume`、`bass`、`brightness` 设置值，关闭夜间模式后按原设置继续工作。

设置接口：

```http
POST /api/v1/night
Content-Type: application/json

{"night":true}
```

关闭：

```json
{"night":false}
```

成功响应：

```json
{
  "code": 0,
  "msg": "ok",
  "data": {
    "night": true
  }
}
```

小程序收到成功响应后，也应该刷新 `GET /api/v1/status`。由于夜间模式需要同步到控制端，建议操作后延迟 200 到 500 ms 再刷新一次状态。

## 小程序页面建议

首页显示：

- 当前输入源：使用 `mode_name`。
- 当前音量：使用 `volume`。
- 当前音色：使用 `tone_name`。
- 夜间模式开关：使用 `night`。
- 网络状态：使用 `network` 和 `device_ip`。

高级控制页提供：

- 音色预设选择器，调用 `POST /api/v1/tone`。
- 夜间模式开关，调用 `POST /api/v1/night`。
- 原有音量、亮度、低音、输入源控制继续使用现有接口。

不要在小程序里直接修改控制端 NV，也不要让 TTP 触摸按键控制音色或夜间模式；这两个功能的唯一产品入口是小程序。

## 错误处理

常见错误：

- `400 invalid tone`：音色值不是 `0~4`，或字符串不在允许列表内。
- `400 missing or invalid night boolean`：夜间模式字段缺失，或不是布尔值/数字。
- `404 not found`：接口路径写错。
- `408 Request Timeout`：请求 body 未完整发送或网络断开。

小程序应在失败时保留原 UI 状态，并通过 `GET /api/v1/status` 重新同步真实设备状态。

## 验收步骤

1. 打开小程序，读取 `GET /api/v1/status`，确认能看到 `tone`、`tone_name`、`night`。
2. 切换音色为 `BASS_BOOST`，播放 SLE 或 DLNA 音频，确认低音增强明显。
3. 重启接收端，重新读取状态，确认 `tone_name` 仍为上一次选择。
4. 打开夜间模式，确认音量上限、低音、灯光亮度都降低。
5. 重启控制端，重新读取状态，确认 `night` 仍为上一次状态。
