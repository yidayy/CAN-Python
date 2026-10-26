#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
can_host.py —— CAN 双关节伺服节点 · PC 端上位机（对齐 can_proto.h v0.2）

与固件的对应关系
----------------
本文件里的每个常量都必须在 E:/Demo/dianji/Core/Inc/can_proto.h 里找得到同名项。
改协议 = 两边一起改，否则第一帧就 CRC 不过。

帧格式（固定 8 字节）
--------------------
    字节   0        1     2      3    4    5    6    7
          0xA0|L   seq   cmd    p0   p1   p2   p3   CRC8
           └ 帧头兼长度 ┘

    [0] 0xA0|L     L = 载荷有效长度 0~4，只可能是 0xA0/0xA1/0xA2/0xA3/0xA4
    [1] seq        发送序号 0~255 循环，收端靠它数丢帧
    [2] cmd        命令码
    [3..6]         载荷，小端序，右侧补 0
    [7] CRC8       CRC-8/SMBUS，poly 0x07 init 0x00，覆盖 [0]~[6]

CAN ID
------
    0x100 + node   命令帧（PC → 板子）
    0x200 + node   应答帧（板子 → PC）

命令 / 应答
-----------
    CMD_PING      0x01  无载荷          → RSP_PONG  0x81  载荷 3B [版本, 节点号, 0]
    CMD_SET_SPEED 0x02  int16 千分比    → 无应答！
    CMD_STOP      0x03  无载荷          → 无应答！
    CMD_GET_STAT  0x04  无载荷          → RSP_STAT  0x82  载荷 4B [int16 duty, rx_ok, 0]

    ⚠️ SET_SPEED 和 STOP 是"发完就不管"的（fire-and-forget），固件不回包。
       想知道到底动没动，必须再发一条 GET_STAT 读回 duty —— 本文件里
       speed 命令就是这么做的，所以你会多看到一行回读。

速度单位是千分比不是 RPM
------------------------
    ±1000 = 占空比 ±100%，固件走的是 MOTOR_SetDuty()，开环。
    编码器闭环（TIM3 + PID）是 v0.3 的事，那时才会换成真正的转速单位。

跑法
----
    # 没有硬件，先用虚拟总线把上位机逻辑跑通
    python can_host.py selftest

    # 看本机 python-can 支持哪些后端
    python can_host.py list

    # 真总线（三种常见适配器，按自己手里的选一个）
    python can_host.py --iface slcan --channel COM3  ping
    python can_host.py --iface gs_usb --channel 0     ping
    python can_host.py --iface pcan --channel PCAN_USBBUS1  ping

    python can_host.py --iface slcan --channel COM3 stat
    python can_host.py --iface slcan --channel COM3 speed 300
    python can_host.py --iface slcan --channel COM3 speed -300
    python can_host.py --iface slcan --channel COM3 stop
    python can_host.py --iface slcan --channel COM3 monitor
    python can_host.py --iface slcan --channel COM3 sweep --hold 1.5

依赖
----
    pip install python-can

    不同适配器的额外要求：
      slcan   : 串口驱动即可，最省事；适配器波特率要和 --bitrate 一致
      gs_usb  : Windows 下要用 Zadig 把适配器驱动换成 WinUSB
      pcan    : 装 PEAK 的 PCAN-Basic 驱动
      virtual : 不用装任何东西，进程内自环，用于 selftest
"""

import argparse
import sys
import threading
import time

try:
    import can
except ImportError:
    sys.exit("缺少 python-can，先跑：pip install python-can")


# ============================================================================
# 协议常量 —— 必须与 can_proto.h 严格一致
# ============================================================================
SOF_BASE = 0xA0
SOF_MASK = 0xF8
FRAME_LEN = 8
MAX_PAYLOAD = 4
PROTO_VERSION = 1

CMD_PING = 0x01
CMD_SET_SPEED = 0x02
CMD_STOP = 0x03
CMD_GET_STAT = 0x04
RSP_PONG = 0x81
RSP_STAT = 0x82

ID_CMD = 0x100
ID_RSP = 0x200

DUTY_LIMIT = 1000          # 千分比满量程
NODE_ID_MAX = 15           # 固件里 CAN_PROTO_Init 做了 & 0x0F

CMD_NAME = {
    CMD_PING: "CMD_PING",
    CMD_SET_SPEED: "CMD_SET_SPEED",
    CMD_STOP: "CMD_STOP",
    CMD_GET_STAT: "CMD_GET_STAT",
    RSP_PONG: "RSP_PONG",
    RSP_STAT: "RSP_STAT",
}


def cmd_name(cmd: int) -> str:
    return CMD_NAME.get(cmd, f"0x{cmd:02X}")


# ============================================================================
# CRC-8/SMBUS —— 与 can_proto.c 的 CAN_PROTO_Crc8() 逐位等价
# ============================================================================
def crc8(data: bytes) -> int:
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


# ============================================================================
# 小端打包 / 解包
# ============================================================================
def put_i16(v: int) -> bytes:
    v &= 0xFFFF
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def get_i16(p: bytes) -> int:
    if len(p) < 2:
        return 0
    u = p[0] | (p[1] << 8)
    return u - 0x10000 if u & 0x8000 else u


# ============================================================================
# 组包 / 解包
# ============================================================================
def encode(node_id: int, cmd: int, payload: bytes, seq: int, is_cmd: bool = True):
    """返回 (can_id, 8 字节帧)。对应固件的 proto_send()。"""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"载荷最长 {MAX_PAYLOAD} 字节，给了 {len(payload)}")

    f = bytearray(FRAME_LEN)
    f[0] = SOF_BASE | len(payload)
    f[1] = seq & 0xFF
    f[2] = cmd & 0xFF
    f[3:3 + len(payload)] = payload
    f[7] = crc8(bytes(f[:7]))
    return ((ID_CMD if is_cmd else ID_RSP) + node_id, bytes(f))


class Frame:
    """一条解出来的协议消息"""
    __slots__ = ("can_id", "node", "seq", "cmd", "payload", "t")

    def __init__(self, can_id, node, seq, cmd, payload, t):
        self.can_id = can_id
        self.node = node
        self.seq = seq
        self.cmd = cmd
        self.payload = payload
        self.t = t

    @property
    def is_rsp(self) -> bool:
        return (self.can_id & 0xF00) == ID_RSP

    def __str__(self):
        return (f"id=0x{self.can_id:03X} {cmd_name(self.cmd):<12}"
                f" seq={self.seq:3d} len={len(self.payload)} "
                f"data={self.payload.hex(' ') or '-'}")


def decode(can_id: int, data: bytes, t: float = 0.0):
    """解析一条裸帧。返回 (Frame, None) 或 (None, 错误桶名)。

    错误桶名与固件 CAN_PROTO_Poll() 的计数桶一一对应，方便两边对数。
    """
    if (can_id & 0xF00) != ID_CMD and (can_id & 0xF00) != ID_RSP:
        return None, "id"

    if len(data) != FRAME_LEN:
        return None, "bad_sof"

    if (data[0] & SOF_MASK) != SOF_BASE:
        return None, "bad_sof"

    plen = data[0] & 0x07
    if plen > MAX_PAYLOAD:
        return None, "bad_len"

    if crc8(bytes(data[:7])) != data[7]:
        return None, "bad_crc"

    return Frame(can_id, can_id & 0x0F, data[1], data[2],
                 bytes(data[3:3 + plen]), t), None


# ============================================================================
# 统计 —— 字段名对齐固件的 CAN_PROTO_Stats_t，方便和串口那行对数
# ============================================================================
class Stats:
    def __init__(self):
        self.tx_frames = 0
        self.tx_fail = 0
        self.rx_frames = 0
        self.rx_ok = 0
        self.rx_bad_sof = 0
        self.rx_bad_len = 0
        self.rx_bad_crc = 0
        self.rx_seq_gap = 0

    def bump_err(self, bucket: str):
        if bucket == "bad_sof":
            self.rx_bad_sof += 1
        elif bucket == "bad_len":
            self.rx_bad_len += 1
        elif bucket == "bad_crc":
            self.rx_bad_crc += 1

    def __str__(self):
        return (f"tx={self.tx_frames} fail={self.tx_fail} | "
                f"rx={self.rx_frames} ok={self.rx_ok} "
                f"sof={self.rx_bad_sof} len={self.rx_bad_len} "
                f"crc={self.rx_bad_crc} gap={self.rx_seq_gap}")


# ============================================================================
# 一个远端节点的代理
# ============================================================================
class CanNode:
    def __init__(self, bus, node_id: int = 1, verbose: bool = False):
        if not (0 < node_id <= NODE_ID_MAX):
            raise ValueError(f"节点号要在 1~{NODE_ID_MAX}，给了 {node_id}")
        self.bus = bus
        self.node = node_id
        self.verbose = verbose
        self.seq = 0
        self.stats = Stats()
        self.last_rx_seq = 0
        self.has_rx = False
        self.seq_buckets = {}       # 按 (方向,节点) 分桶，绕开固件的"已知限制②"

    # ---------------------------------------------------------------- 发送
    def send(self, cmd: int, payload: bytes = b"", is_cmd: bool = True) -> int:
        can_id, frame = encode(self.node, cmd, payload, self.seq, is_cmd)
        msg = can.Message(arbitration_id=can_id, data=frame, is_extended_id=False)
        seq = self.seq
        try:
            self.bus.send(msg)
            self.stats.tx_frames += 1
        except can.CanError as e:
            self.stats.tx_fail += 1
            raise RuntimeError(f"发送失败：{e}") from e

        if self.verbose:
            print(f"  TX id=0x{can_id:03X} {cmd_name(cmd):<12} seq={seq:3d} "
                  f"data={payload.hex(' ') or '-'}")
        self.seq = (self.seq + 1) & 0xFF
        return seq

    # ---------------------------------------------------------------- 接收
    def recv(self, timeout: float = 0.5):
        """读一条并解析。返回 Frame 或 None（超时/被过滤）。"""
        raw = self.bus.recv(timeout)
        if raw is None:
            return None

        self.stats.rx_frames += 1
        frame, err = decode(raw.arbitration_id, bytes(raw.data), raw.timestamp)
        if err:
            self.stats.bump_err(err)
            if self.verbose and err != "id":
                print(f"  RX 丢弃 id=0x{raw.arbitration_id:03X} "
                      f"data={bytes(raw.data).hex(' ')} → {err}")
            return None

        # 序号连续性按 (方向, 节点) 分桶 —— 固件那边共用一个桶，
        # 双节点同时发时会互相当成丢帧，这里分开统计，避免误报。
        key = (frame.is_rsp, frame.node)
        prev = self.seq_buckets.get(key)
        if prev is not None and frame.seq != (prev + 1) & 0xFF:
            self.stats.rx_seq_gap += 1
            if self.verbose:
                print(f"  序号跳变 {key}: {prev} → {frame.seq}")
        self.seq_buckets[key] = frame.seq

        self.stats.rx_ok += 1
        if self.verbose:
            print(f"  RX {frame}")
        return frame

    def wait_for(self, want_cmd: int, timeout: float = 1.0):
        """一直读到超时，找第一条 cmd 匹配的应答。"""
        deadline = time.monotonic() + timeout
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                return None
            frame = self.recv(remain)
            if frame is not None and frame.cmd == want_cmd:
                return frame

    # ---------------------------------------------------------------- 业务命令
    def ping(self, timeout: float = 1.0):
        """探活。返回 (版本, 节点号) 或 None。"""
        self.send(CMD_PING)
        rsp = self.wait_for(RSP_PONG, timeout)
        if rsp is None:
            return None
        pl = rsp.payload + bytes(3)
        return pl[0], pl[1]

    def set_speed(self, permille: int):
        """下发占空比千分比。无应答，发完即返回。"""
        if not -DUTY_LIMIT <= permille <= DUTY_LIMIT:
            raise ValueError(f"千分比要在 ±{DUTY_LIMIT}，给了 {permille}")
        self.send(CMD_SET_SPEED, put_i16(permille))

    def stop(self):
        """急停。无应答。"""
        self.send(CMD_STOP)

    def get_stat(self, timeout: float = 1.0):
        """读遥测。返回 dict 或 None。"""
        self.send(CMD_GET_STAT)
        rsp = self.wait_for(RSP_STAT, timeout)
        if rsp is None:
            return None
        pl = rsp.payload + bytes(4)
        return {
            "duty": get_i16(pl[0:2]),
            "rx_ok": pl[2],
            "flags": pl[3],
        }


# ============================================================================
# 模拟从节点 —— 行为逐条复刻 main.c 的 can_cmd_handler()
# 只给 selftest 用，真总线上用不到
# ============================================================================
class MockNode(threading.Thread):
    def __init__(self, bus, node_id: int = 1, die_after: int = 0):
        super().__init__(daemon=True)
        self.bus = bus
        self.node = node_id
        self.duty = 0
        self.rx_ok = 0
        self.tx_seq = 0
        self._stop = threading.Event()
        self.die_after = die_after        # >0 时收够这么多帧就装死，用来测超时

    def run(self):
        while not self._stop.is_set():
            raw = self.bus.recv(0.1)
            if raw is None:
                continue
            frame, err = decode(raw.arbitration_id, bytes(raw.data))
            if err or frame.is_rsp:
                continue                     # 固件同样不应答 RSP_*
            if frame.node != self.node:
                continue

            self.rx_ok += 1
            if self.die_after and self.rx_ok >= self.die_after:
                self._stop.set()
                continue

            if frame.cmd == CMD_PING:
                self._reply(RSP_PONG, bytes([PROTO_VERSION, self.node, 0]))
            elif frame.cmd == CMD_SET_SPEED:
                self.duty = get_i16(frame.payload)      # 无应答，和固件一致
            elif frame.cmd == CMD_STOP:
                self.duty = 0                            # 无应答
            elif frame.cmd == CMD_GET_STAT:
                self._reply(RSP_STAT, put_i16(self.duty)
                            + bytes([self.rx_ok & 0xFF, 0]))

    def _reply(self, cmd, payload):
        can_id, frame = encode(self.node, cmd, payload, self.tx_seq, is_cmd=False)
        self.tx_seq = (self.tx_seq + 1) & 0xFF
        try:
            self.bus.send(can.Message(arbitration_id=can_id, data=frame,
                                      is_extended_id=False))
        except can.CanError:
            pass

    def halt(self):
        self._stop.set()


# ============================================================================
# 总线打开
# ============================================================================
def _chan(v: str):
    """gs_usb / kvaser 要 int，slcan 要字符串。自动判断。"""
    try:
        return int(v)
    except (TypeError, ValueError):
        return v


def open_bus(iface: str, channel: str, bitrate: int):
    kwargs = {"interface": iface, "channel": _chan(channel)}
    if iface != "virtual":
        kwargs["bitrate"] = bitrate
    try:
        return can.Bus(**kwargs)
    except Exception as e:
        tip = ""
        if iface == "pcan" and "PCANBasic" in str(e):
            tip = ("\n这是驱动没装，不是代码问题。\n"
                   "  装 PEAK 驱动包：https://www.peak-system.com/quick/DrvSetup\n"
                   "  装完拔插一次模块，跑 python can_host.py diag 复查。")
        elif iface == "gs_usb":
            tip = "\ngs_usb 在 Windows 上要先用 Zadig 把驱动换成 WinUSB。"
        sys.exit(f"打不开总线（iface={iface} channel={channel}）：{e}{tip}\n"
                 f"体检：python can_host.py diag")


# ============================================================================
# 子命令
# ============================================================================
def cmd_ping(args, node: CanNode):
    t0 = time.monotonic()
    r = node.ping(args.timeout)
    dt = (time.monotonic() - t0) * 1000

    if r is None:
        print(f"节点 {node.node} 无应答（等了 {args.timeout:.1f}s）")
        print("排查顺序：")
        print("  1. 固件里 CAN_BSP_LOOPBACK 是不是还等于 1 —— 那样帧根本不发上总线")
        print("  2. 适配器和板子是不是都配的 500kbps")
        print("  3. CANH/CANL 有没有接反、总线上有没有 120Ω 终端电阻")
        return 1

    ver, nid = r
    print(f"节点 {nid} 在线，协议版本 v{ver}，往返 {dt:.1f} ms")
    if nid != node.node:
        print(f"注意：应答的节点号是 {nid}，和你指定的 {node.node} 不一致")
    return 0


def cmd_speed(args, node: CanNode):
    node.set_speed(args.permille)
    print(f"已下发 {args.permille:+d} 千分比（≈ {args.permille / 10:.1f}% 占空比）")

    if args.no_check:
        return 0

    st = node.get_stat(args.timeout)
    if st is None:
        print("回读失败：没收到 RSP_STAT，速度可能没生效")
        return 1
    print(f"回读 duty = {st['duty']:+d}")
    if st["duty"] != args.permille:
        print(f"警告：回读值和下发值不一致（差 {st['duty'] - args.permille}）")
        return 1
    return 0


def cmd_stop(args, node: CanNode):
    node.stop()
    print("已下发 CMD_STOP")
    if not args.no_check:
        st = node.get_stat(args.timeout)
        if st is None:
            print("回读失败：没收到 RSP_STAT")
            return 1
        print(f"回读 duty = {st['duty']:+d}" + ("" if st["duty"] == 0 else "  ← 没停住！"))
        return 0 if st["duty"] == 0 else 1
    return 0


def cmd_stat(args, node: CanNode):
    st = node.get_stat(args.timeout)
    if st is None:
        print(f"节点 {node.node} 无应答")
        return 1
    print(f"节点 {node.node} 遥测")
    print(f"  duty   = {st['duty']:+d} 千分比 ({st['duty'] / 10:+.1f}%)")
    print(f"  rx_ok  = {st['rx_ok']}  (固件端已接收的协议帧数，只取低 8 位)")
    print(f"  flags  = 0x{st['flags']:02X}")
    print(f"  本机统计  {node.stats}")
    return 0


def cmd_monitor(args, node: CanNode):
    """持续打印总线上的协议帧，Ctrl+C 退出。"""
    csv = open(args.csv, "w", encoding="utf-8", newline="") if args.csv else None
    if csv:
        csv.write("t,t_can,can_id,node,dir,cmd,seq,len,payload\n")
        print(f"记录到 {args.csv}")

    print("监听中，Ctrl+C 停止")
    t0 = time.monotonic()
    n = 0
    try:
        while True:
            if args.duration and time.monotonic() - t0 > args.duration:
                break
            frame = node.recv(0.5)
            if frame is None:
                continue
            n += 1
            print(f"[{frame.t:9.3f}] {frame}")
            if csv:
                csv.write(f"{time.monotonic() - t0:.3f},{frame.t:.6f},"
                          f"0x{frame.can_id:03X},{frame.node},"
                          f"{'RSP' if frame.is_rsp else 'CMD'},"
                          f"{cmd_name(frame.cmd)},{frame.seq},"
                          f"{len(frame.payload)},{frame.payload.hex()}\n")
    except KeyboardInterrupt:
        print()
    finally:
        if csv:
            csv.close()
            print(f"已记录 {n} 帧")
    print(f"共 {n} 帧 | {node.stats}")
    return 0


def cmd_sweep(args, node: CanNode):
    """按序列跑一遍速度，每档保持 hold 秒并回读。做阶跃响应曲线用。"""
    seq = []
    for i in range(abs(args.step), DUTY_LIMIT + 1, abs(args.step)):
        seq.append(-i)
    seq.append(0)
    for i in range(abs(args.step), DUTY_LIMIT + 1, abs(args.step)):
        seq.append(i)
    seq.append(0)
    if args.reverse:
        seq = seq[::-1]

    print(f"共 {len(seq)} 档，每档 {args.hold}s，总时长约 {len(seq) * args.hold:.0f}s")
    print(f"{'#':>3} {'下发':>7} {'回读':>7} {'误差':>6}")
    for i, v in enumerate(seq, 1):
        node.set_speed(v)
        time.sleep(args.hold)
        st = node.get_stat(args.timeout)
        if st is None:
            print(f"{i:>3} {v:>+7d} {'超时':>7}")
            continue
        print(f"{i:>3} {v:>+7d} {st['duty']:>+7d} {st['duty'] - v:>+6d}")

    node.stop()
    print(f"结束，已发 STOP | {node.stats}")
    return 0


def cmd_list(args, node):
    print(f"python-can {can.__version__}")
    print("本机可用的后端：")
    for iface, desc in can.interfaces.BACKENDS.items():
        print(f"  {iface:<14} {desc}")
    print()
    print("常用对照：")
    print("  slcan    串口转 CAN（CANable 刷 slcan 固件、USB-CAN 模块）   --channel COM3")
    print("  gs_usb   CANable 刷 candleLight 固件 / 各种 gs_usb 适配器   --channel 0")
    print("  pcan     PEAK PCAN-USB                                      --channel PCAN_USBBUS1")
    print("  virtual  不用硬件，进程内自环                                 --channel vcan0")
    return 0


def cmd_diag(args, _node=None):
    """逐层体检：python-can → PCANBasic.dll → 驱动认不认板子。

    出问题时按这个顺序卡在哪一层，一眼能看出来，不用瞎猜。
    """
    import platform
    print("=" * 62)
    print("环境体检")
    print("=" * 62)
    print(f"python      {platform.python_version()}")
    print(f"python-can  {can.__version__}")
    print(f"平台        {platform.system()} {platform.machine()}")

    ok = True

    print("\n[1] pcan 后端")
    has_pcan = "pcan" in can.interfaces.BACKENDS
    print(f"    {'有' if has_pcan else '没有'}")
    if not has_pcan:
        print("    → python-can 装得不完整，重装：pip install --force-reinstall python-can")
        return 1

    print("\n[2] PCANBasic.dll 能不能加载")
    dll_ok = False
    try:
        from can.interfaces.pcan.basic import PCANBasic
        PCANBasic()
        dll_ok = True
        print("    能加载")
    except Exception as e:
        ok = False
        print(f"    加载失败：{e}")
        print("    → 装 PEAK 驱动包：https://www.peak-system.com/quick/DrvSetup")
        print("    → 装完拔插一次模块再来")

    if not dll_ok:
        print("\n" + "=" * 62)
        print("卡在第 2 层：DLL 缺失。这一层不通，后面都不用看。")
        return 1

    print("\n[3] 驱动认不认这块板子（枚举已连接的 PCAN 通道）")
    cfgs = []
    try:
        cfgs = can.detect_available_configs(interfaces="pcan")
    except Exception as e:
        print(f"    枚举失败：{e}")

    if cfgs:
        for c in cfgs:
            print(f"    找到 {c}")
    else:
        ok = False
        print("    一个通道都没有")
        print("    → DLL 在，但驱动没认出设备。设备管理器里看看是不是还有黄色感叹号。")
        print("    → 克隆板可能不被官方驱动接受，找卖家要配套驱动。")

    if cfgs:
        print("\n[4] 按 500 kbit/s 试着初始化一下")
        chan = cfgs[0].get("channel", "PCAN_USBBUS1")
        try:
            bus = can.Bus(interface="pcan", channel=chan, bitrate=500000)
            print(f"    初始化成功（{chan} @ 500k）")
            bus.shutdown()
            print("\n一切就绪，下一步：")
            print(f"    python can_host.py --iface pcan --channel {chan} ping")
        except Exception as e:
            ok = False
            print(f"    初始化失败：{e}")
            print("    → 通道被别的程序占着？把 PCAN-View 关掉再试。")

    print("\n" + "=" * 62)
    print("体检通过" if ok else "有问题，看上面标 → 的那几行")
    print("=" * 62)
    return 0 if ok else 1


# ============================================================================
# selftest —— 不用任何硬件，把整条链路验一遍
# ============================================================================
def cmd_selftest(args, _node=None):
    iface, chan = "virtual", "selftest0"
    pc_bus = can.Bus(interface=iface, channel=chan)
    dev_bus = can.Bus(interface=iface, channel=chan)
    mock = MockNode(dev_bus, node_id=1)
    mock.start()

    node = CanNode(pc_bus, node_id=1)
    failed = 0

    def check(name, cond, detail=""):
        nonlocal failed
        if cond:
            print(f"  [PASS] {name}")
        else:
            failed += 1
            print(f"  [FAIL] {name}  {detail}")

    print("=" * 66)
    print("can_host 自测（虚拟总线，不需要硬件）")
    print("=" * 66)

    try:
        print("\n[1] CRC-8/SMBUS 归零")
        check('ASCII "123456789" → 0xF4', crc8(b"123456789") == 0xF4,
              f"得到 0x{crc8(b'123456789'):02X}")

        print("\n[2] 组包字段位置")
        cid, f = encode(1, CMD_SET_SPEED, put_i16(-300), seq=5)
        check("CAN ID = 0x101", cid == 0x101, f"得到 0x{cid:03X}")
        check("帧头 = 0xA2（len 2）", f[0] == 0xA2, f"得到 0x{f[0]:02X}")
        check("seq 落在 [1]", f[1] == 5)
        check("cmd 落在 [2]", f[2] == CMD_SET_SPEED)
        check("载荷从 [3] 起，小端", f[3:5] == put_i16(-300), f"得到 {f[3:5].hex()}")
        check("CRC 落在 [7]", f[7] == crc8(bytes(f[:7])))

        print("\n[3] 解包与拒收")
        fr, err = decode(0x101, f)
        check("自家帧能解开", err is None and fr is not None)
        check("解出的 duty 是 -300", fr and get_i16(fr.payload) == -300)

        bad = bytearray(f); bad[3] ^= 0x01
        check("翻 1 bit → bad_crc", decode(0x101, bytes(bad))[1] == "bad_crc")
        bad = bytearray(f); bad[0] = 0xA5
        bad[7] = crc8(bytes(bad[:7]))
        check("len=5 → bad_len", decode(0x101, bytes(bad))[1] == "bad_len")
        bad = bytearray(f); bad[0] = 0x5A
        check("帧头不对 → bad_sof", decode(0x101, bytes(bad))[1] == "bad_sof")
        check("DLC 不足 8 → bad_sof", decode(0x101, bytes(f[:5]))[1] == "bad_sof")
        check("ID 0x7FF 不认", decode(0x7FF, bytes(f))[1] == "id")

        print("\n[4] PING（有应答）")
        r = node.ping(1.0)
        check("收到 RSP_PONG", r is not None, str(r))
        check("版本号 = 1", r and r[0] == PROTO_VERSION, str(r))
        check("节点号 = 1", r and r[1] == 1, str(r))

        print("\n[5] SET_SPEED（无应答，靠 GET_STAT 回读）")
        node.set_speed(420)
        st = node.get_stat(1.0)
        check("回读 duty = 420", st is not None and st["duty"] == 420, str(st))

        node.set_speed(-1000)
        st = node.get_stat(1.0)
        check("负向满量程 -1000", st is not None and st["duty"] == -1000, str(st))

        print("\n[6] STOP（无应答，靠 GET_STAT 回读）")
        node.stop()
        st = node.get_stat(1.0)
        check("STOP 后 duty = 0", st is not None and st["duty"] == 0, str(st))

        print("\n[7] 越界与非法输入必须在 PC 侧就拦住")
        for bad_v in (1001, -1001, 99999):
            try:
                node.set_speed(bad_v)
                check(f"拒绝 {bad_v}", False, "居然发出去了")
            except ValueError:
                check(f"拒绝 {bad_v}", True)

        try:
            CanNode(pc_bus, node_id=0)
            check("拒绝节点号 0", False)
        except ValueError:
            check("拒绝节点号 0", True)

        print("\n[8] 解析端统计桶对不对")
        s = node.stats
        check("rx_ok > 0", s.rx_ok > 0, str(s))
        check("bad_crc = 0", s.rx_bad_crc == 0, str(s))
        check("bad_len = 0", s.rx_bad_len == 0, str(s))
        check("tx_frames > 0", s.tx_frames > 0, str(s))

        print("\n[9] 对端掉线必须超时返回，不能挂死")
        mock.halt()
        mock.join(1.0)
        t0 = time.monotonic()
        r = node.ping(0.5)
        dt = time.monotonic() - t0
        check("掉线后 ping 返回 None", r is None, str(r))
        check("超时时间在 0.4~1.2s 之间", 0.4 <= dt <= 1.2, f"实际 {dt:.2f}s")

    finally:
        mock.halt()
        pc_bus.shutdown()
        dev_bus.shutdown()

    print("\n" + "=" * 66)
    if failed:
        print(f"结果：{failed} 项失败")
    else:
        print("结果：全部通过")
    print("=" * 66)
    return 1 if failed else 0


# ============================================================================
# CLI
# ============================================================================
def build_parser():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--iface", default="slcan",
                        help="总线后端：slcan / gs_usb / pcan / virtual（默认 slcan）")
    common.add_argument("--channel", default="COM3",
                        help="通道：slcan 填 COM3，gs_usb 填 0，pcan 填 PCAN_USBBUS1")
    common.add_argument("--bitrate", type=int, default=500000,
                        help="波特率，必须和固件一致（默认 500000）")
    common.add_argument("--node", type=int, default=1,
                        help="目标节点号 1~15（默认 1）")
    common.add_argument("--timeout", type=float, default=1.0,
                        help="等应答的秒数（默认 1.0）")
    common.add_argument("-v", "--verbose", action="store_true",
                        help="打印每一帧收发")

    p = argparse.ArgumentParser(
        description="CAN 双关节伺服节点上位机（对齐 can_proto.h v0.2）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="先跑 `python can_host.py selftest`，不接硬件就能验证上位机逻辑。")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="列出本机可用的总线后端")
    sub.add_parser("diag", help="逐层体检：DLL → 驱动 → 通道 → 初始化")
    sub.add_parser("selftest", help="虚拟总线上跑完整流程，不需要硬件")

    sub.add_parser("ping", parents=[common], help="探活")
    sub.add_parser("stat", parents=[common], help="读一次遥测")

    sp = sub.add_parser("speed", parents=[common], help="下发速度（千分比）")
    sp.add_argument("permille", type=int, help="-1000 ~ +1000")
    sp.add_argument("--no-check", action="store_true", help="发了就走，不回读")

    sp = sub.add_parser("stop", parents=[common], help="急停")
    sp.add_argument("--no-check", action="store_true", help="发了就走，不回读")

    sp = sub.add_parser("monitor", parents=[common], help="监听总线上的协议帧")
    sp.add_argument("--duration", type=float, default=0, help="监听多少秒，0 = 一直听")
    sp.add_argument("--csv", default="", help="把收到的帧存成 CSV")

    sp = sub.add_parser("sweep", parents=[common], help="按序列跑速度，看跟不跟得上")
    sp.add_argument("--step", type=int, default=300, help="每档步长（默认 300）")
    sp.add_argument("--hold", type=float, default=1.5, help="每档保持秒数（默认 1.5）")
    sp.add_argument("--reverse", action="store_true", help="从正到负跑")

    return p


HANDLERS = {
    "ping": cmd_ping,
    "speed": cmd_speed,
    "stop": cmd_stop,
    "stat": cmd_stat,
    "monitor": cmd_monitor,
    "sweep": cmd_sweep,
    "list": cmd_list,
    "diag": cmd_diag,
    "selftest": cmd_selftest,
}


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass

    args = build_parser().parse_args()

    if args.cmd == "selftest":
        return cmd_selftest(args)
    if args.cmd == "list":
        return cmd_list(args, None)
    if args.cmd == "diag":
        return cmd_diag(args, None)

    bus = open_bus(args.iface, args.channel, args.bitrate)
    node = CanNode(bus, args.node, verbose=args.verbose)
    try:
        rc = HANDLERS[args.cmd](args, node)
    except RuntimeError as e:
        print(f"出错：{e}")
        rc = 1
    except KeyboardInterrupt:
        print("\n已中断")
        rc = 130
    finally:
        if args.cmd != "monitor":
            print(f"统计  {node.stats}")
        bus.shutdown()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
