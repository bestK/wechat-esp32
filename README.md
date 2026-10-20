# wechat-esp32

把 ESP32-S3 变成一块只做一件事的「微信通知手表」：通过 Apple ANCS
（Apple Notification Center Service）接收 iPhone 的系统通知，过滤出微信后
把发件人和正文打印到串口。

已在 **ESP32-S3 (16 MB flash)** + iPhone 实机验证通过。

## 它是怎么工作的

```
iPhone (ANCS 服务端)                        ESP32-S3 (ANCS 客户端)
        │                                            │
        │  ← 广播里带 ANCS 128-bit Service Solicitation
        │  ──────── 主动连接 ──────────────────────→  │
        │  ←─────── 配对 / 绑定 (SC + Bond) ───────→  │
        │  ←─────── GATTC 复用这条物理链路 ────────→  │
        │  ── Notification Source: 新通知 UID ─────→  │
        │  ←─ Control Point: 请求 App/标题/正文 ───→  │
        │  ── Data Source: 分片返回属性 ───────────→  │
```

关键点：**是 iPhone 连我们，不是我们扫 iPhone**。广播包里带 ANCS 的
service solicitation（AD type `0x15`），iOS 看到才会把通知服务开放出来。
这也是为什么不需要用户去设置里找「共享系统通知」开关——实测 iOS 17
默认就开放了。

## 快速开始

```bash
pio run
pio run -t upload          # 端口在 platformio.ini 里改
pio device monitor -b 115200
```

然后：

1. iPhone **设置 → 蓝牙**，找到 `WeChat-Watch`，点它配对
2. 配对后**锁屏**（微信在前台时 iOS 不产生系统通知）
3. 让别人给你发一条微信

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

## 工程结构

```
wechat-esp32/
├── platformio.ini        ESP32-S3 目标、16MB flash 覆盖
├── sdkconfig.defaults    BLE-only + 关键特性开关
├── partitions.csv        7MB app 分区
├── main/
│   ├── ancs_client.c     ANCS GATT 客户端（核心，约 600 行）
│   ├── ancs_client.h     对外接口与通知结构体
│   └── app_main.c        BLE 栈初始化 + 微信过滤打印
├── tools/
│   └── ancs_wechat.py    Windows/bleak 版，用来先验证 iPhone 是否肯给通知
└── README.md
```

## 实现要点

### 广播里必须有 service solicitation

```c
s_adv[off++] = 1 + 16;
s_adv[off++] = 0x15;                  // List of 128-bit Service Solicitation UUIDs
memcpy(s_adv + off, ANCS_SERVICE_UUID, 16);
```

没有这一段，iPhone 不会主动连接，ANCS 服务也不会出现。

### ESP32-S3 必须关掉 BLE 5.0 特性

`esp_ble_gattc_open()` 在 IDF 里被 `#if (BLE_42_FEATURE_SUPPORT == TRUE)` 包着，
而 S3 上 `CONFIG_BT_BLE_50_FEATURES_SUPPORTED` 默认是 `y`，两者在 Bluedroid 里互斥：

```ini
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n
```

不关掉会直接编译报「隐式声明 `esp_ble_gattc_open`」。

### `CONFIG_BT_GATTS_ENABLE` 要保留 `y`

虽然这个工程不写任何 GATT 服务，但 Bluedroid 只有在 server 角色被编译进去时
才注册 GAP service（0x1800），而 iOS 连接外设时会去读它。

### conn_id 从 0 开始

Bluedroid 的第一个连接 `conn_id` 就是 `0`。用 `if (!s_conn_id)` 判断「未连接」
会把第一个连接直接挡掉，而且**没有任何报错**——配对、加密、MTU 全部成功，
就是不搜索服务。这个坑排查了很久，现在用独立的 `s_have_conn` 标志。

### 积压通知去重

iOS 每次重连都会把通知中心里没清除的通知重新推一遍。工程里用 NVS 存最近
16 条 `UID + 内容哈希`，命中就跳过：

- 同 UID + 同内容 → 跳过（重放）
- 不同 UID + 同内容 → 显示（不同消息）
- 同 UID + 不同内容 → 显示（iOS 复用了 UID）

只存 UID 不够：iOS 在通知被清除后可能复用 UID。

### 一次推送多条通知

iPhone 会连续推多条，日志里能看到 uid 3~10 在几毫秒内全部到达。所以
category 不能只用一个全局变量缓存，要用 UID 索引的小表（`s_pending[8]`），
否则后到的通知会覆盖前一条的 category。

## 已知限制

**微信在前台时收不到通知。** iOS 不为前台 App 产生系统通知，ANCS 就没有事件。
这是协议限制，不是固件问题。

**配对被降级到 Legacy Just Works。** 板子没有屏幕和键盘（`ESP_IO_CAP_NONE`），
iOS 无法验证配对码，所以 `ESP_LE_AUTH_REQ_SC_BOND` 实际走了 Legacy 配对：

```
W (43519) BT_SMP: FOR LE SC LTK IS USED INSTEAD OF STK
```

对 ANCS 没有影响。要真正启用 Secure Connections，需要加屏幕或按键来显示/输入
6 位配对码，并改成 `ESP_IO_CAP_DISPLAY_ONLY`。

**正文长度受限。** 标题 64 字节、正文 256 字节（UTF-8 下中文约 21 / 85 字），
超出部分由 iOS 截断。

**只处理 `EventID = Added`。** 通知的修改和撤回事件被忽略。

## 调试：先在 PC 上验证

`tools/ancs_wechat.py` 用 `bleak` 在 Windows 上做同样的 ANCS 客户端。
如果 PC 上都收不到通知，说明问题在手机设置而不是固件。

```bash
pip install bleak
python tools/ancs_wechat.py --address AA:BB:CC:DD:EE:FF
```

## 硬件

默认 `esp32-s3-devkitc-1`（板型定义是 N8/8MB，但实际芯片 16MB，已在
`platformio.ini` 里用 `board_upload.flash_size = 16MB` 覆盖）。
换板子改 `board` 和 `board_upload.flash_size`。

## 从哪来的

从 `xes-esp32/xiaomiao-framework` 的 ANCS 部分抽取并重写，去掉了屏幕、按键、
蜂鸣器、WiFi 配网和 ANT BMS 模拟，只保留 ANCS 一条链路。
