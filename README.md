# wechat-esp32

ESP32-S3 同时承担三件事：

- 通过 Apple ANCS 接收 iPhone 系统通知，过滤微信（`com.tencent.xin`）后输出到串口
- 作为 BLE HID 键盘，可由 HTTP 接口驱动向手机输入文字
- 提供 SoftAP 配网热点与 HTTP 控制接口

ANCS 已在 ESP32-S3（16 MB flash）与 iPhone 上验证；HID 键盘已在 Windows 10
上验证（主机收到的按键与发送内容一致），iPhone 端输入尚未实机确认。

## 工作原理

```
iPhone (ANCS 服务端)                        ESP32-S3 (ANCS 客户端)
        │                                            │
        │  ← 广播携带 ANCS 128-bit Service Solicitation
        │  ──────── 发起连接 ──────────────────────→  │
        │  ←─────── 配对 / 绑定 (SC + Bond) ───────→  │
        │  ←─────── GATTC 复用物理链路 ────────────→  │
        │  ── Notification Source: 新通知 UID ─────→  │
        │  ←─ Control Point: 请求 App/标题/正文 ───→  │
        │  ── Data Source: 分片返回属性 ───────────→  │
```

连接由 iPhone 发起，设备不扫描手机。广播包携带 ANCS service solicitation
（AD type `0x15`），iOS 收到后开放通知服务。

## 快速开始

```bash
pio run
pio run -t upload          # 端口在 platformio.ini 中配置
pio device monitor -b 115200
```

操作步骤：

1. iPhone 设置 → 蓝牙，选择 `WeChat-Watch` 配对
2. 配对后锁屏（微信在前台时 iOS 不产生系统通知）
3. 发送一条微信消息
4. 连接 `WeChat-Watch` 热点，打开 http://192.168.4.1/ 配置局域网

串口输出：

```
I (41391) ancs: link up, addr_type=0
I (41393) ancs: gattc open, conn=0
I (41635) ancs: auth ok (mode=0x9, key_present=0)
I (47361) ancs: mtu=517
I (52135) ancs: searching ANCS service on conn=0
I (52135) ancs: chars: notif=40 control=37 data=43
I (52138) ancs: ready, waiting for notifications
I (31848) ancs: added: uid=10 category=4
I (31969) app: ================ WeChat (social) ================
I (31970) app:   from: 南方。
I (31970) app:   text: 1
I (31972) app: =============================================
```

## HTTP 接口

连接 `WeChat-Watch` 热点后访问 `http://192.168.4.1`。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 配网页面 |
| GET | `/api/scan` | 扫描结果；首次调用返回 `"scanning":true`，稍后重试 |
| POST | `/api/wifi` | `{"ssid":"...","password":"..."}`，连接失败重试 5 次后放弃，热点保持在线 |
| GET | `/api/status` | STA 连接状态与 IP |
| POST | `/api/forget` | 清除已保存的 WiFi |
| GET | `/hid/status` | `{"connected":true}` 表示已有 HID 主机 |
| GET | `/hid/type?text=...` | 输入文字，返回按键数；仅支持 US 布局 ASCII，含非 ASCII 返回 400 |
| GET | `/hid/line?text=...` | 同上，末尾追加回车 |

```bash
curl -X POST http://192.168.4.1/api/wifi -H "Content-Type: application/json" \
     -d '{"ssid":"MyWiFi","password":"secret"}'
curl "http://192.168.4.1/hid/line?text=hello"
```

恢复出厂（清除 WiFi 配置）：运行中长按 BOOT 5 秒，或 5 秒内连按 RST 3 次。

## 工程结构

```
wechat-esp32/
├── platformio.ini        ESP32-S3 目标、16MB flash 覆盖
├── sdkconfig.defaults    BLE-only 与特性开关
├── partitions.csv        7MB app 分区
├── main/
│   ├── ancs_client.c     ANCS GATT 客户端
│   ├── ancs_client.h     接口与通知结构体
│   ├── hid/              ESP-IDF HID device profile（HOGP）
│   ├── hid_keyboard.c    BLE HID 键盘：ASCII → HID usage
│   ├── hid_http.c        /hid/* HTTP 接口
│   ├── wifi_portal.c     SoftAP 配网页面与 /api/* 接口
│   ├── reset_button.c    BOOT 长按 / RST 连按恢复出厂
│   └── app_main.c        BLE 栈初始化与微信过滤
├── tools/
│   └── ancs_wechat.py    Windows/bleak 版客户端，用于验证 iPhone 侧
└── README.md
```

## 实现要点

### 广播必须包含 service solicitation

```c
s_adv[off++] = 1 + 16;
s_adv[off++] = 0x15;                  // List of 128-bit Service Solicitation UUIDs
memcpy(s_adv + off, ANCS_SERVICE_UUID, 16);
```

缺少该字段时 iPhone 不会主动连接，ANCS 服务也不会出现。

### ESP32-S3 需关闭 BLE 5.0 特性

`esp_ble_gattc_open()` 在 IDF 中由 `#if (BLE_42_FEATURE_SUPPORT == TRUE)` 控制，
ESP32-S3 上 `CONFIG_BT_BLE_50_FEATURES_SUPPORTED` 默认为 `y`，两者在 Bluedroid
中互斥：

```ini
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n
```

未关闭时编译报隐式声明 `esp_ble_gattc_open`。

### `CONFIG_BT_GATTS_ENABLE` 需保留 `y`

工程未定义 GATT 服务，但 Bluedroid 仅在编译 server 角色时注册 GAP service
（0x1800），iOS 连接外设时会读取该服务。

### conn_id 从 0 开始

Bluedroid 的第一个连接 `conn_id` 为 `0`。使用 `if (!s_conn_id)` 判断未连接会
拦截首个连接，且不产生任何错误：配对、加密、MTU 协商均成功，服务搜索不会执行。
代码中使用独立的 `s_have_conn` 标志区分。

### 积压通知去重

iOS 每次重连会重新推送通知中心内未清除的通知。工程使用 NVS 保存最近 16 条
`UID + 内容哈希`，命中则跳过：

| 条件 | 行为 |
|---|---|
| 同 UID + 同内容 | 跳过（重放） |
| 不同 UID + 同内容 | 输出（不同消息） |
| 同 UID + 不同内容 | 输出（iOS 复用 UID） |

仅保存 UID 不足以区分：iOS 在通知清除后可能复用 UID。

### 单次推送多条通知

iPhone 会连续推送多条通知，实测 uid 3~10 在数毫秒内全部到达。category 不能
使用单一全局变量缓存，否则后到的通知会覆盖前一条的值。代码使用 UID 索引的
表（`s_pending[8]`）。

### HID 服务声明不能要求加密

ESP-IDF 示例中 HID 服务声明的权限为 `ESP_GATT_PERM_READ_ENCRYPTED`，违反核心规范
（服务声明须无需认证即可读取）。iOS 先配对后发现服务，不受影响；Windows 在加密
完成前做主服务发现，配对后只枚举出 0x1800 / 0x1801 / 0x180F，不加载键盘驱动。
已改为 `ESP_GATT_PERM_READ`。

Windows 配对后若仍无 HID 设备，重新发现一次 GATT 服务（如重新连接）即可触发
`HID\{00001812-...}` 节点创建。

### httpd 默认只有 8 个 URI handler

配网页与 `/api/*` 共 9 个 handler，再加 `/hid/*` 3 个，超出默认值后
`httpd_register_uri_handler()` 返回 `ESP_ERR_HTTPD_HANDLERS_FULL`，只打日志、不影响
启动，后注册的接口返回 404。`start_http()` 中设置 `cfg.max_uri_handlers = 16`。

### STA 重连需要上限

密码错误时 STA 每约 3.4 秒重连一次，射频被持续占用，SoftAP 信标发不出去，
热点搜索不到。重连 5 次后放弃，热点保持在线。

## 已知限制

**微信在前台时无通知。** iOS 不为前台应用产生系统通知，ANCS 不产生事件。

**配对降级为 Legacy Just Works。** 设备无屏幕与键盘（`ESP_IO_CAP_NONE`），
iOS 无法验证配对码，`ESP_LE_AUTH_REQ_SC_BOND` 实际走 Legacy 配对：

```
W (43519) BT_SMP: FOR LE SC LTK IS USED INSTEAD OF STK
```

不影响 ANCS 功能。启用 Secure Connections 需增加屏幕或按键输入 6 位配对码，
并改用 `ESP_IO_CAP_DISPLAY_ONLY`。

**正文长度受限。** 标题 64 字节、正文 256 字节（UTF-8 下中文约 21 / 85 字），
超出部分由 iOS 截断。

**仅处理 `EventID = Added`。** 通知修改与撤回事件被忽略。

## 待办

### HID

- 在 iPhone 上确认输入（需停留在文本框内）
- 中文输入：HID 只能发送 usage id，需借助剪贴板或输入法方案

### RPA

启用可解析私有地址，避免设备 MAC 固定暴露：

```ini
CONFIG_BT_BLE_RPA_SUPPORTED=y
CONFIG_BT_BLE_RPA_TIMEOUT=900
```

并在初始化时调用 `esp_ble_gap_config_local_privacy(true)`，广播参数改用
`BLE_ADDR_TYPE_RPA_PUBLIC`。

需要注意：RPA 与当前绑定关系绑定于 IRK。更换地址类型后，iPhone 侧需要删除
旧配对记录并重新绑定，否则 `ESP_GAP_BLE_AUTH_CMPL_EVT` 会失败。

## 调试

`tools/ancs_wechat.py` 使用 `bleak` 在 Windows 上实现同一 ANCS 客户端，
用于排除手机侧配置问题。

```bash
pip install bleak
python tools/ancs_wechat.py --address AA:BB:CC:DD:EE:FF
```

## 硬件

默认目标 `esp32-s3-devkitc-1`（板型定义为 N8/8MB，实际芯片为 16MB，已在
`platformio.ini` 中通过 `board_upload.flash_size = 16MB` 覆盖）。
更换硬件时修改 `board` 与 `board_upload.flash_size`。
