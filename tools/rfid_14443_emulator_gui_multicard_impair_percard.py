#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RFID ISO/IEC 14443 (MFRC522 UART 封包格式) 虚拟串口模拟器（GUI版）
- 支持：SEARCH/ANTICOLL/SELECT/AUTH/READ/WRITE（0x46/0x47/0x48/0x4A/0x4B/0x4C）
- 新增：多卡（多 UID）与“弱网络/弱硬件”现象模拟：乱序发包、发包延迟/抖动、丢包（可选复制包、收包丢包）

协议依据：ISO_IEC14443.docx 中的“数据通讯协议/封包格式/命令字”。（帧头02，帧尾03，转义0x10，校验为逐字节累加最后1字节）
"""

import time
import threading
import queue
import random
from dataclasses import dataclass, field
from typing import Optional, Dict, List, Tuple

import serial

try:
    import tkinter as tk
    from tkinter import ttk, scrolledtext, messagebox
except Exception as e:
    raise RuntimeError("缺少 tkinter（通常 Python 自带）。如果是精简版 Python，请安装带 tkinter 的版本。") from e


# -----------------------------
# 协议常量（与实验箱协议一致）
# -----------------------------
START = 0x02
STOP = 0x03
ESC = 0x10

CMD_SEARCH = 0x46
CMD_ANTICOLL = 0x47
CMD_SELECT = 0x48
CMD_AUTH = 0x4A
CMD_READ = 0x4B
CMD_WRITE = 0x4C

S50_TYPE_BYTE = 0x08  # 仅用于 SELECT 返回“卡容量”字节（实验箱示例里是 0x08）
TAGTYPE_S50 = b"\x04\x00"  # 0x0400 = Mifare One(S50)

DEFAULT_UIDS = [
    bytes.fromhex("B7D2FF79"),
    bytes.fromhex("11223344"),
    bytes.fromhex("A1B2C3D4"),
]
DEFAULT_KEYA = bytes.fromhex("FFFFFFFFFFFF")


# -----------------------------
# 帧编解码
# -----------------------------
def u8(x: int) -> int:
    return x & 0xFF


def checksum(addr_le2: bytes, length: int, cmd: int, data: bytes) -> int:
    # 从“模块地址”到“数据域最后一字节”逐字节累加取最后1字节
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


def read_frames(ser: serial.Serial, stop_event: threading.Event):
    buf = bytearray()
    in_frame = False
    esc = False  # 上一个字节是否是 ESC

    while not stop_event.is_set():
        b = ser.read(1)
        if not b:
            continue
        v = b[0]

        if not in_frame:
            if v == START:
                buf[:] = [v]
                in_frame = True
                esc = False
            continue

        # in_frame
        buf.append(v)

        if esc:
            # 当前字节是被转义的数据字节，不能拿来判断 STOP
            esc = False
            continue

        if v == ESC:
            esc = True
            continue

        if v == STOP:
            yield bytes(buf)
            buf[:] = []
            in_frame = False
            esc = False


# -----------------------------
# 卡模拟（多卡）
# -----------------------------
@dataclass
class S50Card:
    uid: bytes
    keya: bytes = DEFAULT_KEYA
    blocks: Dict[int, bytearray] = field(default_factory=dict)
    authed_blocks: set = field(default_factory=set)

    def __post_init__(self):
        if not self.blocks:
            self.blocks = {i: bytearray(16) for i in range(64)}
            # 默认演示数据（块1/块2）
            self.blocks[1][:] = b"HELLO_RFID_BLOCK1"[0:16]
            self.blocks[2][:] = b"DEMO_DATA_BLOCK2!"[0:16]

    def clear_user_blocks(self):
        self.blocks[1][0:16] = b"\x00" * 16
        self.blocks[2][0:16] = b"\x00" * 16
        self.authed_blocks.clear()

    @staticmethod
    def sector_of_block(block: int) -> int:
        return block // 4

    @staticmethod
    def blocks_in_sector(sector: int):
        base = sector * 4
        return range(base, base + 4)

    def auth_a(self, block: int, key: bytes) -> int:
        # 返回 status：0x00 ok，0x02 key fail，0x03 block invalid
        if block >= 64:
            return 0x03
        if key != self.keya:
            return 0x02
        sector = self.sector_of_block(block)
        for b in self.blocks_in_sector(sector):
            self.authed_blocks.add(b)
        return 0x00

    def read_block(self, block: int) -> Tuple[int, bytes]:
        if block >= 64:
            return 0x03, b""
        if block not in self.authed_blocks:
            return 0x02, b""
        return 0x00, bytes(self.blocks[block][:16])

    def write_block(self, block: int, payload16: bytes) -> int:
        if block >= 64:
            return 0x03
        if block not in self.authed_blocks:
            return 0x02
        self.blocks[block][0:16] = payload16[0:16]
        return 0x00

    def reset_session(self):
        self.authed_blocks.clear()


class MultiCardEmu:
    """
    说明：
    - “是否有卡”改为 **每张卡独立 present**（可单独放卡/收卡），GUI 也保留“全部放卡/收卡”快捷键
    - 只有「启用且在场」的卡 >=1 时，才会响应 ANTICOLL/SELECT/AUTH/READ/WRITE
    - 当存在多卡时，ANTICOLL 按模式返回某一张 UID（轮询/随机/固定）
    - SELECT 成功后，会锁定 selected_uid；后续 AUTH/READ/WRITE 都作用在该卡上
    """
    def __init__(self):
        self.cards: List[S50Card] = [S50Card(uid=u, keya=DEFAULT_KEYA) for u in DEFAULT_UIDS]
        self.present_flags: List[bool] = [False for _ in self.cards]  # 每张卡是否在场
        self.enabled: List[bool] = [True for _ in self.cards]
        self.mode: str = "round_robin"  # round_robin | random | fixed
        self.fixed_index: int = 0

        self._rr_idx = 0
        self.selected_uid: Optional[bytes] = None

    def set_present_all(self, present: bool):
        """快捷：全部放卡/收卡"""
        self.present_flags = [present for _ in self.cards]
        if not present:
            # 所有卡收走：清除选择与认证会话
            self.selected_uid = None
            for c in self.cards:
                c.reset_session()

    def set_card_present(self, index: int, present: bool):
        """单独控制某张卡是否在场"""
        if index < 0 or index >= len(self.cards):
            return
        self.present_flags[index] = present
        if not present:
            # 如果当前选中的卡被收走，取消选择并清 session
            uid = self.cards[index].uid
            if self.selected_uid == uid:
                self.selected_uid = None
            self.cards[index].reset_session()

    def toggle_card_present(self, index: int):
        if index < 0 or index >= len(self.cards):
            return
        self.set_card_present(index, not self.present_flags[index])

    def active_indices(self) -> List[int]:
        """返回：启用且在场的卡索引"""
        out: List[int] = []
        for i, (en, pres) in enumerate(zip(self.enabled, self.present_flags)):
            if en and pres:
                out.append(i)
        return out

    def get_enabled_cards(self) -> List[S50Card]:
        """兼容旧名字：返回启用且在场的卡"""
        out: List[S50Card] = []
        for c, en, pres in zip(self.cards, self.enabled, self.present_flags):
            if en and pres:
                out.append(c)
        return out

    def clear_user_blocks_all(self):
        for c in self.cards:
            c.clear_user_blocks()

    def choose_for_anticoll(self) -> Optional[S50Card]:
        if len(self.get_enabled_cards()) == 0:
            return None
        enabled = self.get_enabled_cards()
        if not enabled:
            return None
        if self.mode == "random":
            return random.choice(enabled)
        if self.mode == "fixed":
            idx = max(0, min(self.fixed_index, len(enabled) - 1))
            return enabled[idx]
        # round_robin
        c = enabled[self._rr_idx % len(enabled)]
        self._rr_idx = (self._rr_idx + 1) % len(enabled)
        return c

    def find_card_by_uid(self, uid: bytes) -> Optional[S50Card]:
        for c, en in zip(self.cards, self.enabled):
            if en and c.uid == uid:
                return c
        return None

    def current_selected_card(self) -> Optional[S50Card]:
        if self.selected_uid is None:
            return None
        return self.find_card_by_uid(self.selected_uid)

    def handle(self, fr: Frame) -> Optional[bytes]:
        """
        返回需要发送的 raw frame。
        若外层要“完全沉默”，应在外层直接不调用/丢弃返回。
        """
        addr, cmd = fr.addr, fr.cmd

        # SEARCH：只要“读写器扫到卡”，通常都会返回 TagType。
        # 这里的简化：present=True 且至少一张 enable 卡 => 返回 S50 TagType
        if cmd == CMD_SEARCH:
            if not self.get_enabled_cards():
                # 无卡：在外层可选沉默；这里给一个错误状态也行，但默认由外层控制
                return build_frame(addr, cmd, bytes([0x01]))
            return build_frame(addr, cmd, bytes([0x00]) + TAGTYPE_S50)

        if cmd == CMD_ANTICOLL:
            if len(self.get_enabled_cards()) == 0:
                return build_frame(addr, cmd, bytes([0x01]))
            if len(fr.data) < 1 or fr.data[0] != 0x04:
                return build_frame(addr, cmd, bytes([0x01]))
            c = self.choose_for_anticoll()
            if c is None:
                return build_frame(addr, cmd, bytes([0x01]))
            return build_frame(addr, cmd, bytes([0x00]) + c.uid)

        if cmd == CMD_SELECT:
            if len(self.get_enabled_cards()) == 0:
                return build_frame(addr, cmd, bytes([0x01]))
            if len(fr.data) != 4:
                return build_frame(addr, cmd, bytes([0x01]))
            uid = fr.data
            c = self.find_card_by_uid(uid)
            if c is None:
                return build_frame(addr, cmd, bytes([0x01]))
            self.selected_uid = uid
            c.reset_session()  # 新一次选卡，通常认证状态重新开始更符合直觉
            return build_frame(addr, cmd, bytes([0x00, S50_TYPE_BYTE]))

        if cmd == CMD_AUTH:
            if len(self.get_enabled_cards()) == 0:
                return build_frame(addr, cmd, bytes([0x01]))
            c = self.current_selected_card()
            if c is None:
                return build_frame(addr, cmd, bytes([0x01]))
            if len(fr.data) != 8:
                return build_frame(addr, cmd, bytes([0x01]))
            mode = fr.data[0]
            block = fr.data[1]
            key = fr.data[2:8]
            if mode != 0x60:  # 只模拟 KeyA
                return build_frame(addr, cmd, bytes([0x01]))
            st = c.auth_a(block, key)
            return build_frame(addr, cmd, bytes([st]))

        if cmd == CMD_READ:
            if len(self.get_enabled_cards()) == 0:
                return build_frame(addr, cmd, bytes([0x01]))
            c = self.current_selected_card()
            if c is None:
                return build_frame(addr, cmd, bytes([0x01]))
            if len(fr.data) != 1:
                return build_frame(addr, cmd, bytes([0x01]))
            block = fr.data[0]
            st, payload = c.read_block(block)
            if st != 0x00:
                return build_frame(addr, cmd, bytes([st]))
            return build_frame(addr, cmd, bytes([0x00]) + payload)

        if cmd == CMD_WRITE:
            if len(self.get_enabled_cards()) == 0:
                return build_frame(addr, cmd, bytes([0x01]))
            c = self.current_selected_card()
            if c is None:
                return build_frame(addr, cmd, bytes([0x01]))
            if len(fr.data) != 17:
                return build_frame(addr, cmd, bytes([0x01]))
            block = fr.data[0]
            payload = fr.data[1:17]
            st = c.write_block(block, payload)
            return build_frame(addr, cmd, bytes([st]))

        # 未知命令
        return build_frame(addr, cmd, bytes([0x7F]))


# -----------------------------
# 异常链路模拟（TX/RX）
# -----------------------------
@dataclass
class TxItem:
    send_at: float
    raw: bytes
    note: str = ""


class Impairment:
    """仅对“设备->上位机”的 TX 回包做仿真（也可选对 RX 做丢包）。"""
    def __init__(self):
        self.base_delay_ms = 80
        self.jitter_ms = 0

        self.tx_drop_rate = 0.0       # 丢包率（0~1）
        self.tx_reorder_rate = 0.0    # 乱序概率（对连续回包做交换）
        self.tx_dup_rate = 0.0        # 复制包概率（重复发送同一帧）
        self.rx_drop_rate = 0.0       # 收包丢包率（忽略收到的命令帧）

    @staticmethod
    def clamp01(x: float) -> float:
        return max(0.0, min(1.0, x))


# -----------------------------
# GUI App
# -----------------------------
class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("RFID 14443 虚拟串口模拟器（GUI版，支持多卡/弱链路）")

        self.log_q: queue.Queue[str] = queue.Queue()
        self.stop_event = threading.Event()

        self.ser: Optional[serial.Serial] = None
        self.worker_rx: Optional[threading.Thread] = None
        self.worker_tx: Optional[threading.Thread] = None

        self.tx_q: "queue.Queue[TxItem]" = queue.Queue()
        self.tx_buf: List[TxItem] = []  # 发送缓冲（用于乱序/延时）

        self.emu = MultiCardEmu()
        self.imp = Impairment()

        self._build_ui()
        self._poll_log()

    # ---------- UI ----------
    def _build_ui(self):
        frm = ttk.Frame(self.root, padding=10)
        frm.grid(row=0, column=0, sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        frm.columnconfigure(0, weight=1)
        frm.rowconfigure(4, weight=1)

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

        # 多卡区
        card_box = ttk.LabelFrame(frm, text="多卡设置（启用/选择/轮询）", padding=10)
        card_box.grid(row=1, column=0, sticky="ew", pady=(10, 0))
        card_box.columnconfigure(3, weight=1)

        ttk.Label(card_box, text="卡列表：").grid(row=0, column=0, sticky="nw")
        self.card_list = tk.Listbox(card_box, height=4, exportselection=False)
        self.card_list.grid(row=0, column=1, sticky="ew", padx=(4, 12))
        self.card_list.bind("<<ListboxSelect>>", lambda e: self._on_card_select())

        btns = ttk.Frame(card_box)
        btns.grid(row=0, column=2, sticky="n")
        self.btn_add = ttk.Button(btns, text="添加随机卡", command=self.on_add_card)
        self.btn_add.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        self.btn_del = ttk.Button(btns, text="删除选中卡", command=self.on_del_card)
        self.btn_del.grid(row=1, column=0, sticky="ew", pady=(0, 6))
        self.btn_edit = ttk.Button(btns, text="编辑UID/KeyA", command=self.on_edit_card)
        self.btn_edit.grid(row=2, column=0, sticky="ew", pady=(0, 6))
        self.btn_toggle = ttk.Button(btns, text="启用/禁用", command=self.on_toggle_card)
        self.btn_toggle.grid(row=3, column=0, sticky="ew")

        self.btn_put_sel = ttk.Button(btns, text="放卡(选中)", command=self.on_put_selected_card)
        self.btn_put_sel.grid(row=4, column=0, sticky="ew", pady=(6, 6))
        self.btn_take_sel = ttk.Button(btns, text="收卡(选中)", command=self.on_take_selected_card)
        self.btn_take_sel.grid(row=5, column=0, sticky="ew")

        mode_box = ttk.Frame(card_box)
        mode_box.grid(row=1, column=1, columnspan=2, sticky="ew", pady=(8, 0))
        ttk.Label(mode_box, text="ANTICOLL返回：").grid(row=0, column=0, sticky="w")
        self.mode_var = tk.StringVar(value="round_robin")
        ttk.Combobox(mode_box, textvariable=self.mode_var, width=14, state="readonly",
                     values=["round_robin", "random", "fixed"]).grid(row=0, column=1, sticky="w", padx=(4, 12))
        ttk.Label(mode_box, text="fixed索引:").grid(row=0, column=2, sticky="w")
        self.fixed_idx = tk.IntVar(value=0)
        ttk.Spinbox(mode_box, from_=0, to=31, textvariable=self.fixed_idx, width=6).grid(row=0, column=3, sticky="w", padx=(4, 0))

        self.selected_label = ttk.Label(card_box, text="当前SELECT锁定：<none>")
        self.selected_label.grid(row=0, column=3, sticky="w")

        # 异常链路区
        imp_box = ttk.LabelFrame(frm, text="弱链路/真机异常模拟（TX=回包）", padding=10)
        imp_box.grid(row=2, column=0, sticky="ew", pady=(10, 0))
        imp_box.columnconfigure(9, weight=1)

        ttk.Label(imp_box, text="基础延迟(ms):").grid(row=0, column=0, sticky="w")
        self.delay_ms = tk.IntVar(value=self.imp.base_delay_ms)
        ttk.Spinbox(imp_box, from_=0, to=5000, textvariable=self.delay_ms, width=8).grid(row=0, column=1, sticky="w", padx=(4, 12))

        ttk.Label(imp_box, text="抖动(ms):").grid(row=0, column=2, sticky="w")
        self.jitter_ms = tk.IntVar(value=self.imp.jitter_ms)
        ttk.Spinbox(imp_box, from_=0, to=2000, textvariable=self.jitter_ms, width=8).grid(row=0, column=3, sticky="w", padx=(4, 12))

        ttk.Label(imp_box, text="丢包率TX(0-1):").grid(row=0, column=4, sticky="w")
        self.tx_drop = tk.DoubleVar(value=self.imp.tx_drop_rate)
        ttk.Entry(imp_box, textvariable=self.tx_drop, width=8).grid(row=0, column=5, sticky="w", padx=(4, 12))

        ttk.Label(imp_box, text="乱序概率TX(0-1):").grid(row=0, column=6, sticky="w")
        self.tx_reorder = tk.DoubleVar(value=self.imp.tx_reorder_rate)
        ttk.Entry(imp_box, textvariable=self.tx_reorder, width=8).grid(row=0, column=7, sticky="w", padx=(4, 12))

        ttk.Label(imp_box, text="复制概率TX(0-1):").grid(row=0, column=8, sticky="w")
        self.tx_dup = tk.DoubleVar(value=self.imp.tx_dup_rate)
        ttk.Entry(imp_box, textvariable=self.tx_dup, width=8).grid(row=0, column=9, sticky="w", padx=(4, 0))

        ttk.Label(imp_box, text="收包丢包RX(0-1):").grid(row=1, column=0, sticky="w", pady=(8, 0))
        self.rx_drop = tk.DoubleVar(value=self.imp.rx_drop_rate)
        ttk.Entry(imp_box, textvariable=self.rx_drop, width=8).grid(row=1, column=1, sticky="w", padx=(4, 12), pady=(8, 0))

        self.silent_when_absent = tk.BooleanVar(value=True)
        ttk.Checkbutton(imp_box, text="收卡后沉默（不回包）", variable=self.silent_when_absent).grid(
            row=1, column=2, columnspan=2, sticky="w", padx=(0, 12), pady=(8, 0)
        )

        self.show_raw_hex = tk.BooleanVar(value=True)
        ttk.Checkbutton(imp_box, text="日志显示原始帧HEX", variable=self.show_raw_hex).grid(
            row=1, column=4, columnspan=2, sticky="w", padx=(0, 12), pady=(8, 0)
        )

        # 控制按钮区
        btn_box = ttk.Frame(frm)
        btn_box.grid(row=3, column=0, sticky="ew", pady=(10, 0))
        btn_box.columnconfigure(8, weight=1)

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

        self.btn_clear_card = ttk.Button(btn_box, text="清空所有卡块1/2", command=self.on_clear_card, state="disabled")
        self.btn_clear_card.grid(row=0, column=5, padx=(0, 8))

        self.btn_reset_sel = ttk.Button(btn_box, text="清除SELECT锁定", command=self.on_reset_select, state="disabled")
        self.btn_reset_sel.grid(row=0, column=6, padx=(0, 8))

        self._refresh_card_list()

        # 日志区
        log_box = ttk.LabelFrame(frm, text="收发包日志", padding=10)
        log_box.grid(row=4, column=0, sticky="nsew", pady=(10, 0))

        self.log = scrolledtext.ScrolledText(log_box, height=18, wrap=tk.WORD)
        self.log.pack(fill=tk.BOTH, expand=True)

        self._log_line("[GUI] 启动完成：先点【连接串口】→ 再点【模拟放卡】")

        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---------- logging ----------
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

    # ---------- card list ----------
    def _refresh_card_list(self):
        self.card_list.delete(0, tk.END)
        for i, c in enumerate(self.emu.cards):
            en = self.emu.enabled[i]
            tag = "ON " if en else "OFF"
            pres = self.emu.present_flags[i]
            ptag = "P" if pres else "-"
            sel = "*" if (self.emu.selected_uid == c.uid) else " "
            self.card_list.insert(tk.END, f"[{tag}][{ptag}]{sel} {i}: UID={c.uid.hex()} KeyA={c.keya.hex()}")
        if self.emu.cards:
            sel = min(len(self.emu.cards) - 1, 0)
            self.card_list.selection_set(sel)
            self.card_list.activate(sel)

    def _on_card_select(self):
        self._update_selected_label()

    def _update_selected_label(self):
        if self.emu.selected_uid is None:
            self.selected_label.configure(text="当前SELECT锁定：<none>")
        else:
            self.selected_label.configure(text=f"当前SELECT锁定：{self.emu.selected_uid.hex()}")

    def _get_selected_index(self) -> Optional[int]:
        try:
            idxs = self.card_list.curselection()
            if not idxs:
                return None
            return int(idxs[0])
        except Exception:
            return None

    def on_add_card(self):
        uid = random.randbytes(4) if hasattr(random, "randbytes") else bytes([random.randrange(256) for _ in range(4)])
        c = S50Card(uid=uid, keya=DEFAULT_KEYA)
        self.emu.cards.append(c)
        self.emu.enabled.append(True)
        self.emu.present_flags.append(False)
        self._refresh_card_list()
        self._log_line(f"[EMU] 已添加卡：UID={uid.hex()} KeyA={DEFAULT_KEYA.hex()}")

    def on_del_card(self):
        idx = self._get_selected_index()
        if idx is None:
            return
        if len(self.emu.cards) <= 1:
            messagebox.showinfo("提示", "至少保留1张卡（防止误删导致无法测试）。")
            return
        uid = self.emu.cards[idx].uid
        del self.emu.cards[idx]
        del self.emu.enabled[idx]
        del self.emu.present_flags[idx]
        if self.emu.selected_uid == uid:
            self.emu.selected_uid = None
        self._refresh_card_list()
        self._update_selected_label()
        self._log_line(f"[EMU] 已删除卡：UID={uid.hex()}")

    def on_toggle_card(self):
        idx = self._get_selected_index()
        if idx is None:
            return
        self.emu.enabled[idx] = not self.emu.enabled[idx]
        self._refresh_card_list()
        self._log_line(f"[EMU] 卡 {idx} 启用状态 -> {self.emu.enabled[idx]}")

    def on_put_selected_card(self):
        idx = self._get_selected_index()
        if idx is None:
            return
        self.emu.set_card_present(idx, True)
        self._refresh_card_list()
        self._log_line(f"[EMU] 卡 {idx} 放卡 -> 在场=True")

    def on_take_selected_card(self):
        idx = self._get_selected_index()
        if idx is None:
            return
        self.emu.set_card_present(idx, False)
        self._refresh_card_list()
        self._log_line(f"[EMU] 卡 {idx} 收卡 -> 在场=False")

    def on_edit_card(self):
        idx = self._get_selected_index()
        if idx is None:
            return
        c = self.emu.cards[idx]

        win = tk.Toplevel(self.root)
        win.title("编辑卡参数")
        win.grab_set()

        ttk.Label(win, text="UID(hex 8位):").grid(row=0, column=0, sticky="w", padx=10, pady=(10, 4))
        uid_var = tk.StringVar(value=c.uid.hex())
        ttk.Entry(win, textvariable=uid_var, width=20).grid(row=0, column=1, padx=10, pady=(10, 4))

        ttk.Label(win, text="KeyA(hex 12位):").grid(row=1, column=0, sticky="w", padx=10, pady=4)
        key_var = tk.StringVar(value=c.keya.hex())
        ttk.Entry(win, textvariable=key_var, width=20).grid(row=1, column=1, padx=10, pady=4)

        def _ok():
            try:
                uid = bytes.fromhex(uid_var.get().strip())
                keya = bytes.fromhex(key_var.get().strip())
                if len(uid) != 4:
                    raise ValueError("UID 必须是4字节（8位hex）")
                if len(keya) != 6:
                    raise ValueError("KeyA 必须是6字节（12位hex）")
            except Exception as e:
                messagebox.showerror("参数错误", str(e))
                return
            old_uid = c.uid
            c.uid = uid
            c.keya = keya
            if self.emu.selected_uid == old_uid:
                self.emu.selected_uid = uid
            self._refresh_card_list()
            self._update_selected_label()
            self._log_line(f"[EMU] 已编辑卡{idx}：UID={uid.hex()} KeyA={keya.hex()}")
            win.destroy()

        ttk.Button(win, text="确定", command=_ok).grid(row=2, column=0, columnspan=2, pady=10)

    # ---------- controls ----------
    def on_clear(self):
        self.log.delete("1.0", tk.END)

    def on_clear_card(self):
        self.emu.clear_user_blocks_all()
        self._log_line("[EMU] 调试：已清空所有卡块1/块2（16字节全0），并清空认证缓存")

    def on_reset_select(self):
        self.emu.selected_uid = None
        self._update_selected_label()
        self._log_line("[EMU] 已清除SELECT锁定（需重新SELECT后才能AUTH/READ/WRITE）")

    def on_put_card(self):
        self.emu.set_present_all(True)
        self._update_selected_label()
        self._log_line("[EMU] 模拟放卡：开始响应命令（SEARCH/ANTICOLL/SELECT/AUTH/READ/WRITE）")

    def on_take_card(self):
        self.emu.set_present_all(False)
        self._update_selected_label()
        self._log_line("[EMU] 收卡：停止响应命令（认证/选卡状态已清空）")

    def _apply_ui_to_params(self):
        # 多卡模式
        self.emu.mode = self.mode_var.get()
        self.emu.fixed_index = int(self.fixed_idx.get())

        # 链路参数
        self.imp.base_delay_ms = max(0, int(self.delay_ms.get()))
        self.imp.jitter_ms = max(0, int(self.jitter_ms.get()))
        self.imp.tx_drop_rate = Impairment.clamp01(float(self.tx_drop.get()))
        self.imp.tx_reorder_rate = Impairment.clamp01(float(self.tx_reorder.get()))
        self.imp.tx_dup_rate = Impairment.clamp01(float(self.tx_dup.get()))
        self.imp.rx_drop_rate = Impairment.clamp01(float(self.rx_drop.get()))

    def on_connect(self):
        if self.ser is not None:
            return

        port = self.port_var.get().strip()
        baud = int(self.baud_var.get())

        try:
            self.ser = serial.Serial(port, baud, timeout=0.2)
        except Exception as e:
            self.ser = None
            messagebox.showerror("串口打开失败", f"{port} @ {baud}\n{e}")
            return

        self.stop_event.clear()
        self.worker_rx = threading.Thread(target=self._serial_rx_loop, daemon=True)
        self.worker_tx = threading.Thread(target=self._serial_tx_loop, daemon=True)
        self.worker_rx.start()
        self.worker_tx.start()

        self.btn_connect.configure(state="disabled")
        self.btn_disconnect.configure(state="normal")
        self.btn_put.configure(state="normal")
        self.btn_take.configure(state="normal")
        self.btn_clear_card.configure(state="normal")
        self.btn_reset_sel.configure(state="normal")

        self._log_line(f"[EMU] 已连接：{port} @ {baud}")
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
        self.btn_reset_sel.configure(state="disabled")

        self._log_line("[EMU] 已断开串口")

    # ---------- impairment scheduling ----------
    def _schedule_tx(self, raw: bytes, note: str = ""):
        self._apply_ui_to_params()

        # 丢包：直接不入队
        if random.random() < self.imp.tx_drop_rate:
            self._log_line(f"[TX] 丢包(drop) {note}".rstrip())
            return

        base = self.imp.base_delay_ms / 1000.0
        jitter = self.imp.jitter_ms / 1000.0
        extra = random.uniform(0.0, jitter) if jitter > 0 else 0.0
        send_at = time.time() + base + extra

        item = TxItem(send_at=send_at, raw=raw, note=note)

        # 乱序：与“最近一个待发送”交换顺序（更像：前一个包慢了，后一个包先到）
        if self.tx_buf and random.random() < self.imp.tx_reorder_rate:
            last = self.tx_buf.pop()  # 交换最后一个
            self.tx_buf.append(item)
            self.tx_buf.append(last)
            self._log_line(f"[TX] 乱序(reorder) 与上一包交换 {note}".rstrip())
        else:
            self.tx_buf.append(item)

        # 复制包：额外再插入一份（发送时间稍后一点点）
        if random.random() < self.imp.tx_dup_rate:
            dup = TxItem(send_at=item.send_at + random.uniform(0.0, 0.05), raw=item.raw, note=(note + " (dup)"))
            self.tx_buf.append(dup)
            self._log_line(f"[TX] 复制(dup) {note}".rstrip())

    # ---------- serial loops ----------
    def _serial_rx_loop(self):
        assert self.ser is not None
        ser = self.ser

        for raw in read_frames(ser, self.stop_event):
            if self.stop_event.is_set():
                break

            self._apply_ui_to_params()

            # RX 丢包：模拟“命令没收到”
            if random.random() < self.imp.rx_drop_rate:
                if self.show_raw_hex.get():
                    self._log_line(f"[RX] 丢包(drop) raw={raw.hex()}")
                else:
                    self._log_line("[RX] 丢包(drop)")
                continue

            fr = parse_frame(raw)
            if not fr.valid:
                if self.show_raw_hex.get():
                    self._log_line(f"[RX] 非法帧: {raw.hex()}")
                else:
                    self._log_line("[RX] 非法帧")
                continue

            if self.show_raw_hex.get():
                self._log_line(f"[RX] cmd=0x{fr.cmd:02X} data={fr.data.hex()}  raw={raw.hex()}")
            else:
                self._log_line(f"[RX] cmd=0x{fr.cmd:02X} data={fr.data.hex()}")

            # 收卡状态：沉默 or 回错误包
            if len(self.emu.get_enabled_cards()) == 0:
                # 没有任何“启用且在场”的卡
                if self.silent_when_absent.get():
                    continue
                resp = build_frame(fr.addr, fr.cmd, bytes([0x01]))
                self._schedule_tx(resp, note="absent->err")
                continue

            # 正常处理
            resp = self.emu.handle(fr)
            self._update_selected_label()

            if resp is None:
                continue

            self._schedule_tx(resp, note=f"cmd=0x{fr.cmd:02X}")

        self._log_line("[EMU] RX线程退出")

    def _serial_tx_loop(self):
        # 专门负责按 send_at 发包（允许队列乱序）
        while not self.stop_event.is_set():
            if self.ser is None:
                time.sleep(0.05)
                continue

            if not self.tx_buf:
                time.sleep(0.01)
                continue

            item = self.tx_buf[0]
            now = time.time()
            if now < item.send_at:
                time.sleep(min(0.02, item.send_at - now))
                continue

            # 到点了，发送队头
            self.tx_buf.pop(0)
            try:
                self.ser.write(item.raw)
                self.ser.flush()
                if self.show_raw_hex.get():
                    self._log_line(f"[TX] {item.raw.hex()} {item.note}".rstrip())
                else:
                    self._log_line("[TX] 已发送")
            except Exception as e:
                self._log_line(f"[TX] 发送失败：{e}")
                break

        self._log_line("[EMU] TX线程退出")

    # ---------- close ----------
    def on_close(self):
        try:
            self.on_disconnect()
        finally:
            self.root.destroy()


def main():
    root = tk.Tk()
    try:
        root.tk.call("tk", "scaling", 1.2)
    except Exception:
        pass
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
