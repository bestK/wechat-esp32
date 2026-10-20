#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Windows ANCS 客户端 —— 通过 BLE 接收 iPhone 的微信通知并在控制台打印。

原理与 ESP32 版 (xm_ancs.c) 一致：作为 BLE GATT 客户端连接已配对的 iPhone，
订阅 Apple Notification Center Service (ANCS)，收到新通知后经 Control Point
拉取详情，重组 Data Source 分片，过滤微信 (com.tencent.xin) 并打印发件人+正文。

依赖：
    pip install bleak

前提（关键）：
    1. 先在 Windows「设置 → 蓝牙和其他设备」里把 iPhone 和电脑配对一次
       （iPhone 只对已绑定 bonded 的中心设备开放 ANCS）。
    2. iPhone「设置 → 通知 → 微信」允许通知 + 横幅，且不在勿扰/专注模式。
    3. 微信退到后台或锁屏时发消息才会产生系统通知（前台不算）。

用法：
    python ancs_wechat.py                # 自动扫描第一个提供 ANCS 的 iPhone
    python ancs_wechat.py --name "我的iPhone"
    python ancs_wechat.py --address AA:BB:CC:DD:EE:FF
    python ancs_wechat.py --all          # 打印所有 App 的通知，不只微信
"""

import argparse
import asyncio
import struct
import sys

from bleak import BleakClient, BleakScanner

# ---- ANCS UUID（128-bit，标准字符串形式）----
ANCS_SERVICE_UUID       = "7905f431-b5ce-4e99-a40f-4b1e122d00d0"
ANCS_NOTIFICATION_SOURCE = "9fbf120d-6301-42d9-8c58-25e699a21dbd"  # notify
ANCS_CONTROL_POINT       = "69d1d8f3-45e1-49a8-9821-9bbdfdaad9d9"  # write
ANCS_DATA_SOURCE         = "22eac6e9-24d6-4bb5-be44-b36ace7c7bfb"  # notify

# ---- Notification Source EventID ----
EVENT_ID_ADDED    = 0
EVENT_ID_MODIFIED = 1
EVENT_ID_REMOVED  = 2

# ---- GetNotificationAttributes 命令与属性 ID ----
CMD_GET_NOTIFICATION_ATTRIBUTES = 0
ATTR_APP_IDENTIFIER = 0   # 无长度字段
ATTR_TITLE          = 1   # 带 2 字节最大长度
ATTR_SUBTITLE       = 2
ATTR_MESSAGE        = 3   # 带 2 字节最大长度

TITLE_MAX   = 64
MESSAGE_MAX = 256

WECHAT_APP_ID = "com.tencent.xin"

# 少数几个类别名，方便日志阅读
CATEGORY_NAMES = {
    0: "Other", 1: "IncomingCall", 2: "MissedCall", 3: "Voicemail",
    4: "Social", 5: "Schedule", 6: "Email", 7: "News",
    8: "HealthFitness", 9: "BusinessFinance", 10: "Location", 11: "Entertainment",
}


class AncsClient:
    def __init__(self, client: BleakClient, wechat_only: bool = True):
        self.client = client
        self.wechat_only = wechat_only
        # 每个通知的 Data Source 响应可能被拆成多个 notify 包，需重组
        self._reasm = bytearray()
        self._reasm_expected_uid = None

    async def start(self):
        # 先订阅 Data Source，确保后续属性响应不丢
        await self.client.start_notify(ANCS_DATA_SOURCE, self._on_data_source)
        await self.client.start_notify(ANCS_NOTIFICATION_SOURCE, self._on_notification_source)
        print("[ancs] 已订阅，等待通知… (Ctrl+C 退出)")

    # ---- Notification Source：新通知到达 ----
    def _on_notification_source(self, _handle, data: bytearray):
        if len(data) < 8:
            return
        event_id, event_flags, category_id, category_count = data[0:4]
        uid = struct.unpack("<I", data[4:8])[0]

        if event_id != EVENT_ID_ADDED:
            return  # 只处理新增通知

        cat = CATEGORY_NAMES.get(category_id, str(category_id))
        print(f"[ancs] 新通知 uid={uid} category={cat}")
        # 异步拉取详情
        asyncio.create_task(self._request_attributes(uid))

    async def _request_attributes(self, uid: int):
        # 组装 GetNotificationAttributes 命令
        cmd = bytearray()
        cmd.append(CMD_GET_NOTIFICATION_ATTRIBUTES)
        cmd += struct.pack("<I", uid)
        cmd.append(ATTR_APP_IDENTIFIER)                 # 无长度
        cmd.append(ATTR_TITLE)
        cmd += struct.pack("<H", TITLE_MAX)
        cmd.append(ATTR_MESSAGE)
        cmd += struct.pack("<H", MESSAGE_MAX)

        # 新一轮响应开始，清空重组缓冲
        self._reasm = bytearray()
        self._reasm_expected_uid = uid
        try:
            await self.client.write_gatt_char(ANCS_CONTROL_POINT, bytes(cmd), response=True)
        except Exception as e:
            print(f"[ancs] control point 写入失败: {e}")

    # ---- Data Source：属性响应（可能分片）----
    def _on_data_source(self, _handle, data: bytearray):
        self._reasm += data
        self._try_parse()

    def _try_parse(self):
        buf = self._reasm
        # 布局: cmd(1) uid(4) 之后是若干 [attr_id(1)][len(2 LE)][value]
        if len(buf) < 5:
            return
        cmd_id = buf[0]
        uid = struct.unpack("<I", buf[1:5])[0]
        if cmd_id != CMD_GET_NOTIFICATION_ATTRIBUTES:
            return

        app_id = title = message = None
        off = 5
        while off + 3 <= len(buf):
            attr_id = buf[off]
            attr_len = struct.unpack("<H", buf[off + 1:off + 3])[0]
            off += 3
            if off + attr_len > len(buf):
                return  # 分片未收全，等下一个包
            value = buf[off:off + attr_len].decode("utf-8", "replace")
            off += attr_len
            if attr_id == ATTR_APP_IDENTIFIER:
                app_id = value
            elif attr_id == ATTR_TITLE:
                title = value
            elif attr_id == ATTR_MESSAGE:
                message = value

        # 三个属性都尝试解析完毕
        self._emit(app_id, title, message)
        self._reasm = bytearray()

    def _emit(self, app_id, title, message):
        if app_id is None:
            return
        if self.wechat_only and app_id != WECHAT_APP_ID:
            print(f"[ancs] 通知来自 {app_id}（忽略）")
            return
        tag = "微信" if app_id == WECHAT_APP_ID else app_id
        print("=" * 40)
        print(f"  【{tag}】")
        print(f"  发件人: {title or '(未知)'}")
        print(f"  内容  : {message or '(空)'}")
        print("=" * 40)


APPLE_COMPANY_ID = 0x004C  # 苹果的 BLE 厂商标识，用来从一堆匿名设备里筛出 iPhone


async def scan_candidates(name=None, address=None):
    """返回候选设备列表（按 RSSI 从强到弱）。
    优先 --address/--name 精确匹配；否则筛出所有 Apple 厂商设备。"""
    print("[scan] 扫描 BLE 设备中（8s）…")
    devices = await BleakScanner.discover(timeout=8.0, return_adv=True)
    items = sorted(devices.values(), key=lambda x: -x[1].rssi)

    # 精确指定优先
    for dev, adv in items:
        if address and dev.address.lower() == address.lower():
            return [dev]
        if name and dev.name and name.lower() in dev.name.lower():
            return [dev]

    # 自动：先看有没有直接广播/邀请 ANCS 的
    for dev, adv in items:
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        if ANCS_SERVICE_UUID in uuids:
            return [dev]

    # 兜底：所有带 Apple 厂商标识的设备，按信号强度排序逐个试
    apple = [dev for dev, adv in items
             if adv.manufacturer_data and APPLE_COMPANY_ID in adv.manufacturer_data]
    if apple:
        print(f"[scan] 找到 {len(apple)} 个 Apple 设备，将按信号强度逐个尝试连接读 ANCS")
        return apple

    print("[scan] 未发现任何 Apple 设备。确认 iPhone 蓝牙已开、且停在蓝牙设置页面。")
    return []


async def try_device(dev, args):
    """连接单个设备，尝试读 ANCS。dev 可以是 BLEDevice 对象或地址字符串。"""
    addr = dev if isinstance(dev, str) else dev.address
    name = dev if isinstance(dev, str) else (dev.name or '?')
    print(f"[conn] 尝试 {addr} ({name}) …")
    try:
        async with BleakClient(addr) as client:
            print("[conn] 已连接")
            try:
                await client.pair()
                print("[pair] 配对/绑定 OK")
            except Exception as e:
                print(f"[pair] pair() 提示（已配对则可忽略）: {e}")

            if client.services.get_service(ANCS_SERVICE_UUID) is None:
                print("[ancs] 此设备没有 ANCS 服务，跳过")
                return False

            print("[ancs] ★ 找到 ANCS 服务！开始订阅")
            ancs = AncsClient(client, wechat_only=not args.all)
            await ancs.start()

            import time as _t
            t0 = _t.monotonic()
            while client.is_connected:
                if args.seconds and (_t.monotonic() - t0) >= args.seconds:
                    print(f"[exit] 到达 {args.seconds}s 运行时限，退出")
                    break
                await asyncio.sleep(0.5)
            print("[conn] 连接已断开")
            return True
    except Exception as e:
        print(f"[conn] 连接失败: {e}")
        return False


async def run(args):
    if args.address:
        # find_device_by_address 只盯目标地址，发现即停，不做全量扫描
        print(f"[conn] 定位已配对设备 {args.address}…")
        dev = await BleakScanner.find_device_by_address(args.address, timeout=15.0)
        if dev is None:
            print("[conn] 15s 内未发现该设备，请确认手机蓝牙已开")
            return 1
        print(f"[conn] 已定位: {dev.name or dev.address}")
        return 0 if await try_device(dev, args) else 2

    cands = await scan_candidates(args.name, None)
    if not cands:
        return 1
    for dev in cands:
        if await try_device(dev, args):
            return 0
    print("[ancs] 所有候选设备都没有开放 ANCS 服务。")
    print("       这通常意味着 iOS 没把通知服务授权给本机——ANCS 对普通 Windows PC 常有此限制。")
    return 2


def main():
    p = argparse.ArgumentParser(description="Windows ANCS 微信通知接收器")
    p.add_argument("--name", help="按名字匹配 iPhone（模糊）")
    p.add_argument("--address", help="按 BLE 地址匹配 iPhone")
    p.add_argument("--all", action="store_true", help="打印所有 App 通知，不只微信")
    p.add_argument("--seconds", type=int, default=0, help="运行秒数上限，0=一直运行")
    args = p.parse_args()
    try:
        rc = asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\n[exit] 已退出")
        rc = 0
    sys.exit(rc)


if __name__ == "__main__":
    main()
