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
  python gw_tool.py COM5 clear-stay              清掉"留在 Bootloader"标志
  python gw_tool.py - encode points.csv out.bin  只把点表编码成文件，不连中控

点表 CSV：每行一个点  表,通道,设备地址,中控地址,传感器寄存器地址   （# 开头是注释）
  例：1x,1,1,0,0   中控 1x 0 = ch1 上地址 1 的设备的 1x 0

协议细节见学习资料《第 6 步学习笔记》《第 7a 步学习笔记》和设计文档 4.2、4.3 节。
注意：reboot / clear-table / enter-boot 之后中控重启，USB 串口会断开再出现，工具会直接退出。

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
TABLES = {"0x": 0, "coil": 0, "1x": 1, "di": 1, "3x": 2, "ir": 2, "4x": 3, "hr": 3}
TABLE_NAMES = ["0x", "1x", "3x", "4x"]
READ_FC = [0x01, 0x02, 0x04, 0x03]
EXCEPTIONS = {1: "非法功能码", 2: "非法地址（没映射 / 只读）", 3: "非法数值（命令 / 包序号 / 长度不对）", 4: "设备故障（CRC 不对 / 点表不合法）"}

SYS_NAMES = ["CMD", "STATE", "FW_VER", "POINT_CNT", "LAST_ERR", "PT_SOURCE"]
PT_SOURCE_TEXT = {0: "内置默认表（Flash 里没有保存的表）", 1: "开机从 Flash 读到的", 2: "这次下发的，已存进 Flash",
                  3: "这次下发的，但存 Flash 失败（重启会丢）", 4: "Flash 里的表坏了，用的内置默认表"}
SYS_CMDS = {"reboot": (0x0001, "重启"), "clear-table": (0x0002, "擦掉保存的点表并重启"),
            "enter-boot": (0x0B00, "设置留在 Bootloader 并重启"), "clear-stay": (0x0A00, "清掉留在 Bootloader 标志")}
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
                buf += self.ser.read(256)
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


def file_header(data: bytes, name: str) -> bytes:
    """32 字节，大端：版本(4) 长度(4) 加载地址(4) CRC32(4) 文件名(16)"""
    return struct.pack(">IIII", 1, len(data), 0, zlib.crc32(data) & 0xFFFFFFFF) + name.encode()[:16].ljust(16, b"\0")


def send_file(gw: Gateway, file_no: int, data: bytes, name: str):
    gw.file_record(file_no, 0, file_header(data, name))
    total = (len(data) + REC_DATA - 1) // REC_DATA
    for i in range(total):
        chunk = data[i * REC_DATA:(i + 1) * REC_DATA]
        gw.file_record(file_no, i + 1, chunk)
        print(f"  包 {i + 1}/{total}：{len(chunk)} 字节")


# ------------------------------------------------------------------ 命令

def cmd_info(gw: Gateway):
    s = gw.read(3, 0xF000, 6)
    print("系统区：")
    for name, v in zip(SYS_NAMES, s):
        extra = ""
        if name == "LAST_ERR":
            extra = f"  （{LAST_ERR_TEXT.get(v, '?')}）"
        elif name == "PT_SOURCE":
            extra = f"  （{PT_SOURCE_TEXT.get(v, '?')}）"
        print(f"  {name:10s} = {v} (0x{v:04X}){extra}")
    st = gw.read(3, 0xF020, 8)
    for ch in (1, 2):
        ok, to, crc, off = st[(ch - 1) * 4:(ch - 1) * 4 + 4]
        print(f"  ch{ch}: 成功 {ok}  超时 {to}  CRC 错 {crc}  掉线设备 {off}   （计数取低 16 位）")


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
        elif cmd in SYS_CMDS:
            code, text = SYS_CMDS[cmd]
            gw.write(3, 0xF000, code)
            print(f"  已发命令 0x{code:04X}：{text}")
            if cmd == "clear-stay":
                time.sleep(0.3)
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
