#!/usr/bin/env python3
import serial
import time
import threading
from dataclasses import dataclass

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


def u8(x):
    return x & 0xFF


def checksum(addr_le2, length, cmd, data):
    s = 0
    s += addr_le2[0]
    s += addr_le2[1]
    s += length
    s += cmd
    for b in data:
        s += b
    return u8(s)


def escape_content(content):
    out = bytearray()
    for b in content:
        if b in (ESC, START, STOP):
            out.append(ESC)
        out.append(b)
    return bytes(out)


def unescape_content(content):
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


def parse_frame(raw):
    if len(raw) < 1 or raw[0] != START or raw[-1] != STOP:
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


def build_frame(addr, cmd, data):
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

    def set_active(self, active):
        self.active = active
        if not active:
            self.authed_blocks.clear()

    def handle(self, fr):
        addr = fr.addr
        cmd = fr.cmd
        if not self.active:
            return build_frame(addr, cmd, bytes([0x01]))
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

    def sector_of_block(self, block):
        return block // 4

    def blocks_in_sector(self, sector):
        base = sector * 4
        return range(base, base + 4)


def read_frames(ser):
    buf = bytearray()
    in_frame = False
    while True:
        b = ser.read(1)
        if not b:
            yield None
            continue
        v = b[0]
        if not in_frame:
            if v == START:
                buf[:] = []
                buf.append(v)
                in_frame = True
        else:
            buf.append(v)
            if v == STOP:
                yield bytes(buf)
                buf[:] = []
                in_frame = False


def input_watcher(card, stop_event):
    while not stop_event.is_set():
        user_in = raw_input().strip() if hasattr(__builtins__, 'raw_input') else input().strip()
        if user_in == '0':
            card.set_active(True)
            print("[EMU] 模拟放卡，开始收发数据")
        elif user_in == '1':
            card.set_active(False)
            print("[EMU] 模拟收卡，停止模拟数据")
        else:
            print("[EMU] 输入0放卡，输入1收卡停止数据")


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9", help="Windows COM port (one end of com0com)")
    ap.add_argument("--baud", type=int, default=115200, help="Baud rate")
    ap.add_argument("--uid", default=DEFAULT_UID.hex(), help="4-byte UID hex")
    ap.add_argument("--keya", default=DEFAULT_KEYA.hex(), help="6-byte KeyA hex")
    args = ap.parse_args()
    card = S50CardEmu(uid=bytes.fromhex(args.uid), keya=bytes.fromhex(args.keya))
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    print("[EMU] Opened %s @ %d" % (ser.port, args.baud))
    print("[EMU] UID=%s KeyA=%s" % (card.uid.hex(), card.keya.hex()))
    print("[EMU] 输入0模拟放卡，输入1模拟收卡停止数据（Ctrl+C退出程序）")
    stop_event = threading.Event()
    watcher = threading.Thread(target=input_watcher, args=(card, stop_event))
    watcher.daemon = True
    watcher.start()
    try:
        for raw in read_frames(ser):
            if stop_event.is_set():
                break
            if raw is None:
                continue
            fr = parse_frame(raw)
            if not fr.valid:
                print("[EMU] RX invalid frame: %s" % raw.hex())
                continue
            print("[EMU] RX cmd=0x%02X data=%s" % (fr.cmd, fr.data.hex()))
            resp = card.handle(fr)
            if resp is None:
                continue
            ser.write(resp)
            ser.flush()
            time.sleep(0.01)
            print("[EMU] TX %s" % resp.hex())
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        ser.close()
        print("[EMU] Closed.")


if __name__ == "__main__":
    main()
