#!/usr/bin/env python3
"""
gw_tool.py - 中控（Modbus 网关）的上位机工具：通过 USB 虚拟串口读写寄存器、下发点表

依赖：pip install pyserial

用法（COM5 换成设备管理器里中控的 COM 口）：
  python gw_tool.py COM5 info                    系统区：版本、点数、最近错误、两个通道的统计
  python gw_tool.py COM5 read 1x 0 3             读：表可以写 0x/1x/3x/4x 或 coil/di/ir/hr
  python gw_tool.py COM5 write 0x 2 1            写一个线圈 / 保持寄存器
  python gw_tool.py COM5 points points.csv       下发点表（0x15 文件记录），中控换上后存进 Flash
  python gw_tool.py COM5 reboot                  重启中控
  python gw_tool.py COM5 clear-table             擦掉 Flash 里保存的点表并重启（重启后用内置默认表）
  python gw_tool.py COM5 enter-boot              下次开机留在 Bootloader（配置区 stay = 1）并重启
  python gw_tool.py COM5 clear-stay              清掉"留在 Bootloader"标志（在 Bootloader 里 = 启动 App）
  python gw_tool.py COM5 run-app                 同 clear-stay：在 Bootloader 里用它启动 App
  python gw_tool.py COM5 forget-app              （只在 Bootloader 里）清掉配置区记的 App 长度和 CRC 再启动：
                                                 用 Keil 下载过 App 后停在 Bootloader（BOOT_REASON = 5）时用
  python gw_tool.py COM5 upgrade central.bin [版本]   升级中控自己：进 Bootloader → 发固件 → 等写完 → 启动新 App
  python gw_tool.py COM5 test-hang               故障测试：ch1 任务卡住 → 看门狗复位
  python gw_tool.py COM5 sensor-cmd 2 3 0x55     通过中控给 ch2 地址 3 的传感器写命令寄存器（0x55 进 Bootloader，0xAA 启动 App）
  python gw_tool.py COM5 upgrade-sensor 2 3 demo_f030_app.bin [版本]
                                                 通过中控升级 ch2 地址 3 的传感器：0x55 → 发固件 → 0xAA
  python gw_tool.py COM5 test-crash              故障测试：每次开机都"死机" → 3 次后留在 Bootloader（run-app 恢复）
  python gw_tool.py - encode points.csv out.bin  只把点表编码成文件，不连中控

点表 CSV：每行一个点  表,通道,设备地址,中控地址,传感器寄存器地址   （# 开头是注释）
  例：1x,1,1,0,0   中控 1x 0 = ch1 上地址 1 的设备的 1x 0

协议细节见学习资料《第 6 步学习笔记》《第 7a 步学习笔记》和设计文档 4.2、4.3 节。
注意：reboot / clear-table / enter-boot 之后中控重启，USB 串口会断开再出现，工具会直接退出；
     upgrade 会自己等串口重新出现（最多 15 秒）。

SPDX-License-Identifier: MIT
Copyright (c) 2026 陈攀
"""
import struct
import sys
import time
import zlib

SLAVE = 1                       # 中控在上行链路上的从机地址
REC_DATA = 224                  # 0x15 每包数据字节数
FILE_NO_POINT_TABLE = 0x0000
FILE_NO_CENTRAL_FW = 0x1000         # 中控自己的固件（设计文档 4.3 节）
APP_ADDR = 0x08040000               # App 的加载地址（第 7b 步起）
SENSOR_APP_ADDR = 0x08020000        # 课程 F030 传感器的 App 地址
SENSOR_APP_MAX = 0x1F800
SENSOR_RAM = (0x20000000, 0x20008000)
TABLES = {"0x": 0, "coil": 0, "1x": 1, "di": 1, "3x": 2, "ir": 2, "4x": 3, "hr": 3}
TABLE_NAMES = ["0x", "1x", "3x", "4x"]
READ_FC = [0x01, 0x02, 0x04, 0x03]
EXCEPTIONS = {1: "非法功能码", 2: "非法地址（没映射 / 只读）", 3: "非法数值（命令 / 包序号 / 长度不对）", 4: "设备故障（CRC 不对 / 点表不合法）"}

SYS_NAMES = ["CMD", "STATE", "FW_VER", "POINT_CNT", "LAST_ERR", "PT_SOURCE", "RESET_RSN", "BOOT_TRIES"]
RESET_TEXT = {0: "未知", 1: "上电", 2: "复位键", 3: "软件复位（命令、升级、进出 Bootloader）", 4: "看门狗"}
PT_SOURCE_TEXT = {0: "内置默认表（Flash 里没有保存的表）", 1: "开机从 Flash 读到的", 2: "这次下发的，已存进 Flash",
                  3: "这次下发的，但存 Flash 失败（重启会丢）", 4: "Flash 里的表坏了，用的内置默认表"}
SYS_CMDS = {"reboot": (0x0001, "重启"), "clear-table": (0x0002, "擦掉保存的点表并重启"),
            "enter-boot": (0x0B00, "设置留在 Bootloader 并重启"), "clear-stay": (0x0A00, "清掉留在 Bootloader 标志"),
            "run-app": (0x0A00, "启动 App（在 App 里 = 清标志）"),
            "forget-app": (0x0A01, "清掉配置区里的 App 信息并启动（用 Keil 下载 App 后停在 Bootloader 时用）"),
            "test-hang": (0x0F01, "故障测试：ch1 任务卡住，看门狗应该在几秒后复位中控"),
            "test-crash": (0x0F02, "故障测试：之后每次开机都\"死机\"，连续 3 次后 Bootloader 应该留下")}
BOOT_NAMES = ["CMD", "STATE", "BOOT_VER", "POINT_CNT", "LAST_ERR", "BOOT_REASON", "UPG_STATE", "APP_VER"]
BOOT_REASON_TEXT = {0: "跳 App", 1: "跳 App（没有配置）", 2: "上位机要求留下（stay = 1）", 3: "没有 App（App 区是空的或没写完）",
                    4: "配置里的长度不对", 5: "App 的 CRC 不对", 6: "App 的向量表不对",
                    7: "App 连续 3 次启动都没稳定下来（看门狗复位），等升级或 run-app"}
UPG_STATE_TEXT = {0: "空闲", 1: "正在收", 2: "收齐了，正在写 Flash", 3: "写好了，等 0x0A00 启动", 4: "失败"}
LAST_ERR_TEXT = {
    0x0000: "无",
    0x0101: "点表：长度不是 8 的倍数", 0x0102: "点表：点太多（> 128）", 0x0103: "点表：表号不对",
    0x0104: "点表：通道不对", 0x0105: "点表：设备地址不对", 0x0106: "点表：中控地址 > 499",
    0x0107: "点表：中控地址重复", 0x0108: "点表：一个通道上设备超过 8 个",
    0x0200: "设备拒绝写入（回了异常码）",
    0x0301: "文件：没收到文件头", 0x0302: "文件：文件头不对", 0x0303: "文件：跳包 / 乱序",
    0x0304: "文件：包长度不对", 0x0305: "文件：CRC32 不对",
    0x0401: "Flash：点表保存失败", 0x0402: "Flash：保存的点表 CRC 不对", 0x0403: "Flash：保存的点表不合法",
    0x0404: "Flash：写配置区失败",
    0x0601: "转发：TARGET 没设或不合法", 0x0602: "转发：传感器没回应", 0x0603: "转发：传感器回了异常码",
    0x0604: "转发：没有文件头 / 跳包", 0x0605: "转发：之前有一包失败了，要从文件头重来",
    0x0606: "转发：第 1 包不像传感器 App（发错文件了？传感器没动）", 0x0607: "转发：整个文件 CRC 不对，最后一包没发（传感器留在 Bootloader）",
    0x0608: "转发：文件长度不对", 0x0609: "转发：请求格式不对",
    0x0501: "升级：加载地址不是 0x08040000（Keil 的 IROM1 改了吗？）", 0x0502: "升级：固件太大或太小",
    0x0503: "升级：向量表不对", 0x0504: "升级：擦 Flash 失败", 0x0505: "升级：写 Flash 失败",
    0x0506: "升级：写完核对 CRC 不对", 0x0507: "升级：写配置区失败",
}


# ------------------------------------------------------------------ Modbus RTU

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def add_crc(pdu_with_addr: bytes) -> bytes:
    return pdu_with_addr + struct.pack("<H", crc16(pdu_with_addr))


class ModbusError(Exception):
    pass


class Gateway:
    def __init__(self, port: str, timeout: float = 1.0):
        import serial                      # 只有连中控时才需要 pyserial
        self.ser = serial.Serial(port, 115200, timeout=0.05)   # 虚拟串口，波特率没有意义
        self.timeout = timeout

    def close(self):
        self.ser.close()

    def _transact(self, req: bytes, expect_len, retries: int = 2) -> bytes:
        """发一帧，收回应。expect_len(收到的前几个字节) → 整帧长度"""
        for attempt in range(retries + 1):
            self.ser.reset_input_buffer()
            self.ser.write(req)
            buf = b""
            deadline = time.time() + self.timeout
            while time.time() < deadline:
                buf += self.ser.read(max(1, self.ser.in_waiting))   # 有多少读多少：不要等满 256 字节（那样每次都白等 50 ms）
                if len(buf) >= 2 and buf[1] & 0x80:            # 异常回应固定 5 字节
                    need = 5
                elif len(buf) >= 3:
                    need = expect_len(buf)
                else:
                    continue
                if len(buf) >= need:
                    rsp = buf[:need]
                    if crc16(rsp[:-2]) != struct.unpack("<H", rsp[-2:])[0]:
                        break                                    # CRC 错：重试
                    if rsp[1] & 0x80:
                        code = rsp[2]
                        raise ModbusError(f"异常 {code:02d}：{EXCEPTIONS.get(code, '?')}")
                    return rsp
            if attempt < retries:
                print(f"  （没收到回应，重试 {attempt + 1}）")
        raise ModbusError("超时：中控没有回应（COM 口对吗？中控在运行吗？）")

    def read(self, table: int, addr: int, count: int):
        fc = READ_FC[table]
        req = add_crc(struct.pack(">BBHH", SLAVE, fc, addr, count))
        rsp = self._transact(req, lambda b: 3 + b[2] + 2)
        data = rsp[3:-2]
        if table <= 1:                                           # 位：每个字节 8 个点，低位在前
            return [(data[i // 8] >> (i % 8)) & 1 for i in range(count)]
        return list(struct.unpack(f">{count}H", data))

    def write(self, table: int, addr: int, value: int):
        if table == 0:
            req = add_crc(struct.pack(">BBHH", SLAVE, 0x05, addr, 0xFF00 if value else 0))
        elif table == 3:
            req = add_crc(struct.pack(">BBHH", SLAVE, 0x06, addr, value))
        else:
            raise ModbusError("1x、3x 是只读的")
        self._transact(req, lambda b: 8)

    def file_record(self, file_no: int, record_no: int, data: bytes):
        if len(data) % 2:
            data += b"\0"                                        # 0x15 以 2 字节为单位
        sub = struct.pack(">BHHH", 6, file_no, record_no, len(data) // 2) + data
        req = add_crc(struct.pack(">BBB", SLAVE, 0x15, len(sub)) + sub)
        self._transact(req, lambda b: 3 + b[2] + 2)              # 回应是请求的原样回显


# ------------------------------------------------------------------ 点表

def parse_csv(path: str) -> list:
    points = []
    with open(path, encoding="utf-8-sig") as f:
        for lineno, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line or line.lower().startswith("table"):
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) != 5:
                raise SystemExit(f"{path} 第 {lineno} 行：要 5 列（表,通道,设备,中控地址,传感器地址），实际 {len(cols)} 列")
            tab = cols[0].lower()
            if tab not in TABLES:
                raise SystemExit(f"{path} 第 {lineno} 行：表只能是 0x/1x/3x/4x 或 coil/di/ir/hr")
            points.append((TABLES[tab],) + tuple(int(c, 0) for c in cols[1:]))
    return points


def encode_points(points) -> bytes:
    """每个点 8 字节，大端：表 通道 设备 保留 中控地址(2) 传感器地址(2)"""
    return b"".join(struct.pack(">BBBBHH", t, ch, dev, 0, gw, da) for t, ch, dev, gw, da in points)


def file_header(data: bytes, name: str, version: int = 1, load_addr: int = 0) -> bytes:
    """32 字节，大端：版本(4) 长度(4) 加载地址(4) CRC32(4) 文件名(16)"""
    return (struct.pack(">IIII", version, len(data), load_addr, zlib.crc32(data) & 0xFFFFFFFF)
            + name.encode()[:16].ljust(16, b"\0"))


def send_file(gw: Gateway, file_no: int, data: bytes, name: str, version: int = 1, load_addr: int = 0):
    gw.file_record(file_no, 0, file_header(data, name, version, load_addr))
    total = (len(data) + REC_DATA - 1) // REC_DATA
    t0 = time.time()
    for i in range(total):
        chunk = data[i * REC_DATA:(i + 1) * REC_DATA]
        gw.file_record(file_no, i + 1, chunk)
        if total <= 10 or (i + 1) % 50 == 0 or i + 1 == total:      # 包多的时候每 50 包打一行
            print(f"  包 {i + 1}/{total}：{len(chunk)} 字节   （{time.time() - t0:.1f} s）")


# ------------------------------------------------------------------ 命令

def cmd_info(gw: Gateway):
    state = gw.read(3, 0xF001, 1)[0]
    if state == 1:                                               # 在 Bootloader 里：系统区不一样
        s = gw.read(3, 0xF000, 8)
        print("系统区（Bootloader）：")
        for name, v in zip(BOOT_NAMES, s):
            extra = {"LAST_ERR": LAST_ERR_TEXT, "BOOT_REASON": BOOT_REASON_TEXT, "UPG_STATE": UPG_STATE_TEXT}.get(name)
            extra = f"  （{extra.get(v, '?')}）" if extra else ""
            print(f"  {name:11s} = {v} (0x{v:04X}){extra}")
        return s
    try:
        s = gw.read(3, 0xF000, 8)
    except ModbusError:                                          # 第 8 步之前的固件只有 6 个
        s = gw.read(3, 0xF000, 6)
    print("系统区：")
    for name, v in zip(SYS_NAMES, s):
        extra = ""
        if name == "LAST_ERR":
            extra = f"  （{LAST_ERR_TEXT.get(v, '?')}）"
        elif name == "PT_SOURCE":
            extra = f"  （{PT_SOURCE_TEXT.get(v, '?')}）"
        elif name == "RESET_RSN":
            extra = f"  （{RESET_TEXT.get(v, '?')}）"
        print(f"  {name:10s} = {v} (0x{v:04X}){extra}")
    st = gw.read(3, 0xF020, 8)
    for ch in (1, 2):
        ok, to, crc, off = st[(ch - 1) * 4:(ch - 1) * 4 + 4]
        print(f"  ch{ch}: 成功 {ok}  超时 {to}  CRC 错 {crc}  掉线设备 {off}   （计数取低 16 位）")
    return s


def reconnect(port: str, what: str, wait: float = 15.0) -> "Gateway":
    """中控重启后 USB 串口会消失再出现：每 0.5 秒试一次，直到能打开、能回应"""
    print(f"  等中控重启（{what}）……", end="", flush=True)
    time.sleep(1.0)
    deadline = time.time() + wait
    while time.time() < deadline:
        try:
            gw = Gateway(port)
            try:
                gw.read(3, 0xF001, 1)
                print(" 连上了")
                return gw
            except ModbusError:
                gw.close()
        except Exception:                                        # 串口还没出现
            pass
        print(".", end="", flush=True)
        time.sleep(0.5)
    raise ModbusError(f"{wait:.0f} 秒内没连上：设备管理器里 {port} 还在吗？")


def set_target(gw: Gateway, ch: int, addr: int):
    gw.write(3, 0xF010, (ch << 8) | addr)


def sensor_cmd(gw: Gateway, ch: int, addr: int, value: int):
    set_target(gw, ch, addr)
    gw.write(3, 0xF011, value)


def last_err_text(gw: Gateway) -> str:
    e = gw.read(3, 0xF004, 1)[0]
    return f"LAST_ERR = 0x{e:04X}（{LAST_ERR_TEXT.get(e, '?')}）"


def cmd_upgrade_sensor(port: str, ch: int, addr: int, path: str, version: int) -> int:
    data = open(path, "rb").read()
    sp, entry = struct.unpack("<II", data[:8])
    print(f"传感器固件 {path}：{len(data)} 字节，CRC32 = 0x{zlib.crc32(data) & 0xFFFFFFFF:08X}，"
          f"目标 ch{ch} 地址 {addr}，版本 {version}")
    print(f"  向量表：栈指针 0x{sp:08X}，复位入口 0x{entry:08X}")
    if not (SENSOR_RAM[0] <= sp <= SENSOR_RAM[1] and SENSOR_APP_ADDR <= (entry & ~1) < SENSOR_APP_ADDR + len(data)):
        print("  ✗ 这不像课程传感器的 App（应该按 0x08020000 编译）。中控也会拒绝，这里先停下")
        return 2
    if len(data) > SENSOR_APP_MAX:
        print(f"  ✗ 太大：传感器 App 区只有 {SENSOR_APP_MAX} 字节")
        return 2

    gw = Gateway(port)
    try:
        print("1. 让传感器进 Bootloader（0x55）")
        sensor_cmd(gw, ch, addr, 0x55)
        time.sleep(2.0)                                          # 传感器写配置、复位、Bootloader 启动

        print("2. 发固件（中控转发；文件头要等传感器擦 Flash，第 1 包会慢一些）")
        gw.timeout = 8.0                                         # 文件头：中控最多等传感器 2 × 3 秒
        t0 = time.time()
        try:
            send_file(gw, FILE_NO_CENTRAL_FW | (ch << 8) | addr, data, "sensor.bin", version, SENSOR_APP_ADDR)
        except ModbusError as e:
            gw.timeout = 1.0
            print(f"   中控拒绝 / 转发失败：{e}")
            print(f"   {last_err_text(gw)}")
            print("传感器留在 Bootloader（或者没被动过），可以重新 upgrade-sensor")
            return 2
        gw.timeout = 1.0
        print(f"   发完 {time.time() - t0:.1f} s")

        print("3. 启动传感器的新 App（0xAA）")
        sensor_cmd(gw, ch, addr, 0xAA)
        time.sleep(2.0)
        cmd_info(gw)
        print("完成。看中控日志里这个设备是否重新 online，读一下它的点确认数据正常")
        return 0
    finally:
        gw.close()


def cmd_upgrade(port: str, path: str, version: int) -> int:
    data = open(path, "rb").read()
    print(f"固件 {path}：{len(data)} 字节，CRC32 = 0x{zlib.crc32(data) & 0xFFFFFFFF:08X}，"
          f"加载地址 0x{APP_ADDR:08X}，版本 {version}")
    if len(data) >= 8:
        sp, entry = struct.unpack("<II", data[:8])
        print(f"  向量表：栈指针 0x{sp:08X}，复位入口 0x{entry:08X}")
        if not APP_ADDR <= (entry & ~1) < APP_ADDR + len(data):
            print("  ⚠ 复位入口不在 0x08040000 之后：这个固件是按别的地址编译的，Bootloader 会拒绝")

    gw = Gateway(port)
    try:
        if gw.read(3, 0xF001, 1)[0] == 0:                        # 1. 在 App 里：先进 Bootloader
            print("1. 中控在运行 App，发 0x0B00 进 Bootloader")
            gw.write(3, 0xF000, 0x0B00)
            gw.close()
            gw = reconnect(port, "进 Bootloader")
        else:
            print("1. 中控已经在 Bootloader 里")
        cmd_info(gw)

        print("2. 发固件（file_no 0x1000）")
        t0 = time.time()
        try:
            send_file(gw, FILE_NO_CENTRAL_FW, data, "central.bin", version, APP_ADDR)
        except ModbusError as e:
            print(f"   Bootloader 拒绝：{e}")
            cmd_info(gw)
            print("Flash 没有被改动（检查在写 Flash 之前）。改好固件再 upgrade；想先回到旧 App：run-app")
            return 2
        except Exception as e:                                   # USB 线被拔、COM 口消失
            print(f"   传输中断：{e}")
            print("固件还没收齐，Bootloader 不会动 Flash：中控留在 Bootloader（stay = 1），旧 App 完好。"
                  "\n插好 USB 后：重新 upgrade，或者 run-app 回到旧 App")
            return 2
        print(f"   发完 {time.time() - t0:.1f} s")

        print("3. 等 Bootloader 写 Flash")
        deadline = time.time() + 20
        while True:
            upg, err = gw.read(3, 0xF006, 1)[0], gw.read(3, 0xF004, 1)[0]
            if upg in (3, 4) or time.time() > deadline:
                break
            time.sleep(0.2)
        print(f"   UPG_STATE = {upg}（{UPG_STATE_TEXT.get(upg, '?')}），LAST_ERR = 0x{err:04X}（{LAST_ERR_TEXT.get(err, '?')}）")
        if upg != 3:
            print("升级没成功：中控留在 Bootloader，旧 App 可能已经被擦了。检查原因后重新 upgrade。")
            return 2

        print("4. 发 0x0A00 启动新 App")
        gw.write(3, 0xF000, 0x0A00)
        gw.close()
        gw = reconnect(port, "启动新 App")
        s = cmd_info(gw)
        if s[1] == 0:
            print(f"升级完成：现在运行的 App 版本 FW_VER = 0x{s[2]:04X}")
            if version and s[2] != version:
                print(f"  ⚠ 命令行给的版本是 0x{version:04X}，App 自己报的是 0x{s[2]:04X}：命令行的版本只是写进文件头的标签，"
                      f"\n    App 报的才是编译进去的。不一致说明 central.bin 不是改完 version.h 以后编译的（没保存？没 F7？）")
            return 0
        print("还在 Bootloader 里：看 BOOT_REASON")
        return 2
    finally:
        gw.close()


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    port, cmd, args = argv[1], argv[2], argv[3:]

    if cmd == "encode":                                          # 不连中控
        data = encode_points(parse_csv(args[0]))
        open(args[1], "wb").write(data)
        print(f"{len(data) // 8} 个点，{len(data)} 字节，CRC32 = 0x{zlib.crc32(data) & 0xFFFFFFFF:08X} → {args[1]}")
        return 0

    if cmd == "upgrade-sensor":
        try:
            return cmd_upgrade_sensor(port, int(args[0], 0), int(args[1], 0), args[2],
                                      int(args[3], 0) if len(args) > 3 else 1)
        except ModbusError as e:
            print(f"错误：{e}")
            return 2

    if cmd == "upgrade":                                         # 会多次断开重连，自己管串口
        try:
            return cmd_upgrade(port, args[0], int(args[1], 0) if len(args) > 1 else 0)
        except ModbusError as e:
            print(f"错误：{e}")
            return 2

    gw = Gateway(port)
    try:
        if cmd == "info":
            cmd_info(gw)
        elif cmd == "read":
            tab, addr = TABLES[args[0].lower()], int(args[1], 0)
            n = int(args[2], 0) if len(args) > 2 else 1
            for i, v in enumerate(gw.read(tab, addr, n)):
                print(f"  {TABLE_NAMES[tab]} {addr + i} = {v}")
        elif cmd == "write":
            tab, addr, val = TABLES[args[0].lower()], int(args[1], 0), int(args[2], 0)
            gw.write(tab, addr, val)
            print(f"  已写 {TABLE_NAMES[tab]} {addr} = {val}（中控收下了；下一轮由通道任务下发到传感器）")
        elif cmd == "points":
            points = parse_csv(args[0])
            data = encode_points(points)
            print(f"下发点表：{len(points)} 个点，{len(data)} 字节，CRC32 = 0x{zlib.crc32(data) & 0xFFFFFFFF:08X}")
            try:
                send_file(gw, FILE_NO_POINT_TABLE, data, "points.bin")
                time.sleep(1.0)                                  # 中控等两个通道停下来再换表，再存 Flash
                print("中控已接受。")
            except ModbusError as e:
                print(f"中控拒绝：{e}")
            cmd_info(gw)
        elif cmd == "sensor-cmd":
            ch, addr, val = int(args[0], 0), int(args[1], 0), int(args[2], 0)
            try:
                sensor_cmd(gw, ch, addr, val)
                print(f"  已写 ch{ch} 地址 {addr} 的命令寄存器 = 0x{val:04X}")
            except ModbusError as e:
                print(f"  失败：{e}；{last_err_text(gw)}")
        elif cmd in SYS_CMDS:
            code, text = SYS_CMDS[cmd]
            gw.write(3, 0xF000, code)
            print(f"  已发命令 0x{code:04X}：{text}")
            if cmd == "clear-stay":
                time.sleep(0.3)
                if gw.read(3, 0xF001, 1)[0] == 0:                # 在 App 里：看一下结果（在 Bootloader 里它会马上重启）
                    cmd_info(gw)
        else:
            print(__doc__)
            return 1
    except ModbusError as e:
        print(f"错误：{e}")
        return 2
    finally:
        gw.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
