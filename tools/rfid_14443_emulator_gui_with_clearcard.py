#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import time
import threading
import queue
from dataclasses import dataclass
from typing import Optional

import serial

try:
    import tkinter as tk
    from tkinter import ttk, scrolledtext, messagebox
except Exception as e:
    raise RuntimeError("缺少 tkinter（通常 Python 自带）。如果是精简版 Python，请安装带 tkinter 的版本。") from e


START = 0x02
STOP = 0x03
ESC = 0x10

CMD_SEARCH = 0x46
CMD_ANTICOLL = 0x47
CMD_SELECT = 0x48
CMD_AUTH = 0x4A
CMD_READ = 0x4B
CMD_WRITE = 0x4C

S50_TYPE_BYTE = 0x08

DEFAULT_UID = bytes.fromhex("B7D2FF79")
DEFAULT_KEYA = bytes.fromhex("FFFFFFFFFFFF")


def u8(x: int) -> int:
    return x & 0xFF


def checksum(addr_le2: bytes, length: int, cmd: int, data: bytes) -> int:
    s = 0
    s += addr_le2[0]
    s += addr_le2[1]
    s += length
    s += cmd
    for b in data:
        s += b
    return u8(s)


def escape_content(content: bytes) -> bytes:
    out = bytearray()
    for b in content:
        if b in (ESC, START, STOP):
            out.append(ESC)
        out.append(b)
    return bytes(out)


def unescape_content(content: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(content):
        b = content[i]
        if b == ESC:
            i += 1
            if i >= len(content):
                break
            out.append(content[i])
        else:
            out.append(b)
        i += 1
    return bytes(out)


@dataclass
class Frame:
    addr: int
    cmd: int
    data: bytes
    valid: bool = True


def parse_frame(raw: bytes) -> Frame:
    if len(raw) < 2 or raw[0] != START or raw[-1] != STOP:
        return Frame(0, 0, b"", valid=False)
    content_escaped = raw[1:-1]
    content = unescape_content(content_escaped)
    if len(content) < 5:
        return Frame(0, 0, b"", valid=False)

    addr_le2 = content[0:2]
    length = content[2]
    cmd = content[3]
    data_len = length - 3
    if data_len < 0:
        return Frame(0, 0, b"", valid=False)

    need = 2 + 1 + 1 + data_len + 1
    if len(content) < need:
        return Frame(0, 0, b"", valid=False)

    data = content[4:4 + data_len]
    chksum_recv = content[4 + data_len]
    chksum_calc = checksum(addr_le2, length, cmd, data)
    if chksum_recv != chksum_calc:
        return Frame(0, 0, b"", valid=False)

    addr = addr_le2[0] | (addr_le2[1] << 8)
    return Frame(addr, cmd, data, valid=True)


def build_frame(addr: int, cmd: int, data: bytes) -> bytes:
    addr_le2 = bytes([addr & 0xFF, (addr >> 8) & 0xFF])
    length = u8(len(data) + 3)
    ch = checksum(addr_le2, length, cmd, data)
    content = addr_le2 + bytes([length, cmd]) + data + bytes([ch])
    raw = bytes([START]) + escape_content(content) + bytes([STOP])
    return raw


class S50CardEmu:
    def __init__(self, uid=DEFAULT_UID, keya=DEFAULT_KEYA):
        self.uid = uid
        self.keya = keya
        self.blocks = {i: bytearray(16) for i in range(64)}
        self.blocks[1][:] = b"HELLO_RFID_BLOCK1"[0:16]
        self.blocks[2][:] = b"DEMO_DATA_BLOCK2!"[0:16]
        self.authed_blocks = set()
        self.active = False

    def set_active(self, active: bool):
        self.active = active
        if not active:
            self.authed_blocks.clear()

    def clear_user_blocks(self):
        """调试用：将块1、块2清零（16字节全0）。"""
        self.blocks[1][0:16] = b"\x00" * 16
        self.blocks[2][0:16] = b"\x00" * 16
        # 清空认证缓存，避免“已认证但数据已变”造成困惑
        self.authed_blocks.clear()

    def sector_of_block(self, block: int) -> int:
        return block // 4

    def blocks_in_sector(self, sector: int):
        base = sector * 4
        return range(base, base + 4)

    def handle(self, fr: Frame) -> Optional[bytes]:
        """
        返回：需要发送的 raw frame；若返回 None 表示“收卡后不回包（完全沉默）”
        """
        addr = fr.addr
        cmd = fr.cmd

        # active=False 时，具体行为由外层决定（沉默 or 回错误包）
        if cmd == CMD_SEARCH:
            return build_frame(addr, cmd, bytes([0x00, 0x04, 0x00]))
        if cmd == CMD_ANTICOLL:
            if len(fr.data) < 1 or fr.data[0] != 0x04:
                return build_frame(addr, cmd, bytes([0x01]))
            return build_frame(addr, cmd, bytes([0x00]) + self.uid)
        if cmd == CMD_SELECT:
            if len(fr.data) != 4 or fr.data != self.uid:
                return build_frame(addr, cmd, bytes([0x01]))
            return build_frame(addr, cmd, bytes([0x00, S50_TYPE_BYTE]))
        if cmd == CMD_AUTH:
            if len(fr.data) != 8:
                return build_frame(addr, cmd, bytes([0x01]))
            mode = fr.data[0]
            block = fr.data[1]
            key = fr.data[2:8]
            if mode != 0x60:
                return build_frame(addr, cmd, bytes([0x01]))
            if key != self.keya:
                return build_frame(addr, cmd, bytes([0x02]))
            if block >= 64:
                return build_frame(addr, cmd, bytes([0x03]))
            sector = self.sector_of_block(block)
            for b in self.blocks_in_sector(sector):
                self.authed_blocks.add(b)
            return build_frame(addr, cmd, bytes([0x00]))
        if cmd == CMD_READ:
            if len(fr.data) != 1:
                return build_frame(addr, cmd, bytes([0x01]))
            block = fr.data[0]
            if block >= 64:
                return build_frame(addr, cmd, bytes([0x03]))
            if block not in self.authed_blocks:
                return build_frame(addr, cmd, bytes([0x02]))
            return build_frame(addr, cmd, bytes([0x00]) + bytes(self.blocks[block][:16]))
        if cmd == CMD_WRITE:
            if len(fr.data) != 17:
                return build_frame(addr, cmd, bytes([0x01]))
            block = fr.data[0]
            payload = fr.data[1:17]
            if block >= 64:
                return build_frame(addr, cmd, bytes([0x03]))
            if block not in self.authed_blocks:
                return build_frame(addr, cmd, bytes([0x02]))
            self.blocks[block][0:16] = payload[0:16]
            return build_frame(addr, cmd, bytes([0x00]))

        return build_frame(addr, cmd, bytes([0x7F]))


def read_frames(ser: serial.Serial, stop_event: threading.Event):
    buf = bytearray()
    in_frame = False
    while not stop_event.is_set():
        b = ser.read(1)
        if not b:
            continue
        v = b[0]
        if not in_frame:
            if v == START:
                buf[:] = [v]
                in_frame = True
        else:
            buf.append(v)
            if v == STOP:
                yield bytes(buf)
                buf[:] = []
                in_frame = False


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("RFID 14443 虚拟串口模拟器（GUI版）")

        self.log_q: queue.Queue[str] = queue.Queue()
        self.stop_event = threading.Event()

        self.ser: Optional[serial.Serial] = None
        self.worker: Optional[threading.Thread] = None

        self.card = S50CardEmu()
        self.card.set_active(False)

        self._build_ui()
        self._poll_log()

    def _build_ui(self):
        frm = ttk.Frame(self.root, padding=10)
        frm.grid(row=0, column=0, sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        frm.columnconfigure(0, weight=1)

        # 串口设置区
        port_box = ttk.LabelFrame(frm, text="串口设置", padding=10)
        port_box.grid(row=0, column=0, sticky="ew")
        port_box.columnconfigure(7, weight=1)

        ttk.Label(port_box, text="Port:").grid(row=0, column=0, sticky="w")
        self.port_var = tk.StringVar(value="COM9")
        ttk.Entry(port_box, textvariable=self.port_var, width=12).grid(row=0, column=1, sticky="w", padx=(4, 12))

        ttk.Label(port_box, text="Baud:").grid(row=0, column=2, sticky="w")
        self.baud_var = tk.IntVar(value=115200)
        ttk.Entry(port_box, textvariable=self.baud_var, width=10).grid(row=0, column=3, sticky="w", padx=(4, 12))

        ttk.Label(port_box, text="UID(hex 8位):").grid(row=0, column=4, sticky="w")
        self.uid_var = tk.StringVar(value=self.card.uid.hex())
        ttk.Entry(port_box, textvariable=self.uid_var, width=12).grid(row=0, column=5, sticky="w", padx=(4, 12))

        ttk.Label(port_box, text="KeyA(hex 12位):").grid(row=0, column=6, sticky="w")
        self.key_var = tk.StringVar(value=self.card.keya.hex())
        ttk.Entry(port_box, textvariable=self.key_var, width=14).grid(row=0, column=7, sticky="w", padx=(4, 0))

        # 速度设置区
        speed_box = ttk.LabelFrame(frm, text="速度/行为", padding=10)
        speed_box.grid(row=1, column=0, sticky="ew", pady=(10, 0))
        speed_box.columnconfigure(7, weight=1)

        ttk.Label(speed_box, text="回包延迟(ms):").grid(row=0, column=0, sticky="w")
        self.delay_ms = tk.IntVar(value=80)
        ttk.Spinbox(speed_box, from_=0, to=2000, textvariable=self.delay_ms, width=8).grid(row=0, column=1, sticky="w", padx=(4, 12))

        ttk.Label(speed_box, text="抖动(ms):").grid(row=0, column=2, sticky="w")
        self.jitter_ms = tk.IntVar(value=0)
        ttk.Spinbox(speed_box, from_=0, to=500, textvariable=self.jitter_ms, width=8).grid(row=0, column=3, sticky="w", padx=(4, 12))

        self.silent_when_absent = tk.BooleanVar(value=True)
        ttk.Checkbutton(speed_box, text="收卡后沉默（不回包）", variable=self.silent_when_absent).grid(
            row=0, column=4, sticky="w", padx=(0, 12)
        )

        self.show_raw_hex = tk.BooleanVar(value=True)
        ttk.Checkbutton(speed_box, text="日志显示原始帧HEX", variable=self.show_raw_hex).grid(
            row=0, column=5, sticky="w", padx=(0, 12)
        )

        # 控制按钮区
        btn_box = ttk.Frame(frm)
        btn_box.grid(row=2, column=0, sticky="ew", pady=(10, 0))
        btn_box.columnconfigure(6, weight=1)

        self.btn_connect = ttk.Button(btn_box, text="连接串口", command=self.on_connect)
        self.btn_connect.grid(row=0, column=0, padx=(0, 8))

        self.btn_disconnect = ttk.Button(btn_box, text="断开串口", command=self.on_disconnect, state="disabled")
        self.btn_disconnect.grid(row=0, column=1, padx=(0, 20))

        self.btn_put = ttk.Button(btn_box, text="模拟放卡", command=self.on_put_card, state="disabled")
        self.btn_put.grid(row=0, column=2, padx=(0, 8))

        self.btn_take = ttk.Button(btn_box, text="收卡", command=self.on_take_card, state="disabled")
        self.btn_take.grid(row=0, column=3, padx=(0, 8))

        self.btn_clear = ttk.Button(btn_box, text="清空日志", command=self.on_clear)
        self.btn_clear.grid(row=0, column=4, padx=(0, 8))

        self.btn_clear_card = ttk.Button(btn_box, text="清空卡信息", command=self.on_clear_card, state="disabled")
        self.btn_clear_card.grid(row=0, column=5, padx=(0, 8))

        # 日志区
        log_box = ttk.LabelFrame(frm, text="收发包日志", padding=10)
        log_box.grid(row=3, column=0, sticky="nsew", pady=(10, 0))
        frm.rowconfigure(3, weight=1)

        self.log = scrolledtext.ScrolledText(log_box, height=18, wrap=tk.WORD)
        self.log.pack(fill=tk.BOTH, expand=True)

        self._log_line("[GUI] 启动完成：先点【连接串口】→ 再点【模拟放卡】")

        # 关闭窗口处理
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def _log_line(self, s: str):
        ts = time.strftime("%H:%M:%S")
        self.log_q.put(f"[{ts}] {s}")

    def _poll_log(self):
        try:
            while True:
                s = self.log_q.get_nowait()
                self.log.insert(tk.END, s + "\n")
                self.log.see(tk.END)
        except queue.Empty:
            pass
        self.root.after(60, self._poll_log)

    def on_clear(self):
        self.log.delete("1.0", tk.END)

    def on_clear_card(self):
        # 只在连接串口后允许操作，避免误以为已生效
        self.card.clear_user_blocks()
        self._log_line("[EMU] 调试：已清空卡信息（块1/块2 -> 16字节全0）")

    def on_put_card(self):
        self.card.set_active(True)
        self._log_line("[EMU] 模拟放卡：开始响应命令（SEARCH/ANTICOLL/SELECT/AUTH/READ/WRITE）")

    def on_take_card(self):
        self.card.set_active(False)
        self._log_line("[EMU] 收卡：停止响应命令（认证状态已清空）")

    def on_connect(self):
        if self.ser is not None:
            return

        port = self.port_var.get().strip()
        baud = int(self.baud_var.get())

        try:
            uid = bytes.fromhex(self.uid_var.get().strip())
            keya = bytes.fromhex(self.key_var.get().strip())
            if len(uid) != 4:
                raise ValueError("UID 必须是4字节（8位hex）")
            if len(keya) != 6:
                raise ValueError("KeyA 必须是6字节（12位hex）")
        except Exception as e:
            messagebox.showerror("参数错误", str(e))
            return

        # 应用新 UID/Key
        self.card.uid = uid
        self.card.keya = keya
        self.card.set_active(False)

        try:
            self.ser = serial.Serial(port, baud, timeout=0.2)
        except Exception as e:
            self.ser = None
            messagebox.showerror("串口打开失败", f"{port} @ {baud}\n{e}")
            return

        self.stop_event.clear()
        self.worker = threading.Thread(target=self._serial_loop, daemon=True)
        self.worker.start()

        self.btn_connect.configure(state="disabled")
        self.btn_disconnect.configure(state="normal")
        self.btn_put.configure(state="normal")
        self.btn_take.configure(state="normal")
        self.btn_clear_card.configure(state="normal")

        self._log_line(f"[EMU] 已连接：{port} @ {baud}")
        self._log_line(f"[EMU] UID={self.card.uid.hex()} KeyA={self.card.keya.hex()}")
        self._log_line("[EMU] 当前为：收卡状态（不响应），点【模拟放卡】开始模拟")

    def on_disconnect(self):
        if self.ser is None:
            return
        self.stop_event.set()
        try:
            self.ser.close()
        except Exception:
            pass
        self.ser = None

        self.btn_connect.configure(state="normal")
        self.btn_disconnect.configure(state="disabled")
        self.btn_put.configure(state="disabled")
        self.btn_take.configure(state="disabled")
        self.btn_clear_card.configure(state="disabled")

        self._log_line("[EMU] 已断开串口")

    def _serial_loop(self):
        assert self.ser is not None
        ser = self.ser

        for raw in read_frames(ser, self.stop_event):
            if self.stop_event.is_set():
                break

            fr = parse_frame(raw)
            if not fr.valid:
                if self.show_raw_hex.get():
                    self._log_line(f"[RX] 非法帧: {raw.hex()}")
                else:
                    self._log_line("[RX] 非法帧")
                continue

            # RX日志
            if self.show_raw_hex.get():
                self._log_line(f"[RX] cmd=0x{fr.cmd:02X} data={fr.data.hex()}  raw={raw.hex()}")
            else:
                self._log_line(f"[RX] cmd=0x{fr.cmd:02X} data={fr.data.hex()}")

            # 收卡状态：沉默 or 回错误包
            if not self.card.active:
                if self.silent_when_absent.get():
                    # 完全不回包
                    continue
                else:
                    # 回一个“执行错误”包：status=0x01（维持你原脚本的风格）
                    resp = build_frame(fr.addr, fr.cmd, bytes([0x01]))
            else:
                resp = self.card.handle(fr)

            if resp is None:
                continue

            # 降速：延迟 + 抖动
            d = max(0, int(self.delay_ms.get()))
            j = max(0, int(self.jitter_ms.get()))
            if d or j:
                # 用 time.time 的小随机抖动，不额外引入 random（避免某些机房环境缺库误会）
                extra = 0
                if j:
                    extra = int((time.time() * 1000) % (j + 1))
                time.sleep((d + extra) / 1000.0)

            try:
                ser.write(resp)
                ser.flush()
            except Exception as e:
                self._log_line(f"[TX] 发送失败：{e}")
                break

            if self.show_raw_hex.get():
                self._log_line(f"[TX] {resp.hex()}")
            else:
                self._log_line("[TX] 已发送")

        self._log_line("[EMU] 串口线程退出")

    def on_close(self):
        try:
            self.on_disconnect()
        finally:
            self.root.destroy()


def main():
    root = tk.Tk()
    # Windows 下更清晰一点
    try:
        root.tk.call("tk", "scaling", 1.2)
    except Exception:
        pass
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
