
# rfid_14443_emulator_active.py
# Active swipe version:
# - Card is ABSENT by default
# - Type "1" + Enter in console => card PRESENT for 5 seconds
# - After 5 seconds without new input => ABSENT again

import serial
import time
import threading
import sys
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

# ---------------- Active swipe gate ----------------
class ActiveSwipeGate:
    def __init__(self, present_seconds=5.0):
        self.present_seconds = present_seconds
        self.present_until = 0.0
        self.lock = threading.Lock()

    def trigger(self):
        with self.lock:
            self.present_until = time.time() + self.present_seconds
            print("[EMU] CARD PRESENT for %.1f seconds" % self.present_seconds)

    def is_present(self):
        with self.lock:
            return time.time() < self.present_until


def console_listener(gate: ActiveSwipeGate):
    print('[EMU] Type "1" + Enter to swipe card (present 5s)')
    while True:
        try:
            line = sys.stdin.readline()
            if not line:
                continue
            if line.strip() == "1":
                gate.trigger()
        except Exception:
            pass


def u8(x): return x & 0xFF


def checksum(addr_le2, length, cmd, data):
    s = addr_le2[0] + addr_le2[1] + length + cmd
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
            if i < len(content):
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
    if raw[0] != START or raw[-1] != STOP:
        return Frame(0, 0, b"", False)
    content = unescape_content(raw[1:-1])
    if len(content) < 5:
        return Frame(0, 0, b"", False)

    addr_le2 = content[0:2]
    length = content[2]
    cmd = content[3]
    data_len = length - 3
    data = content[4:4 + data_len]
    chk = content[4 + data_len]

    if chk != checksum(addr_le2, length, cmd, data):
        return Frame(0, 0, b"", False)

    addr = addr_le2[0] | (addr_le2[1] << 8)
    return Frame(addr, cmd, data, True)


def build_frame(addr, cmd, data):
    addr_le2 = bytes([addr & 0xFF, (addr >> 8) & 0xFF])
    length = u8(len(data) + 3)
    chk = checksum(addr_le2, length, cmd, data)
    content = addr_le2 + bytes([length, cmd]) + data + bytes([chk])
    return bytes([START]) + escape_content(content) + bytes([STOP])


class S50CardEmu:
    def __init__(self):
        self.uid = DEFAULT_UID
        self.keya = DEFAULT_KEYA
        self.blocks = {i: bytearray(16) for i in range(64)}
        self.blocks[1][:] = b"HELLO_RFID_BLOCK1"[0:16]
        self.blocks[2][:] = b"DEMO_DATA_BLOCK2!"[0:16]
        self.authed = set()

    def handle(self, fr: Frame, present: bool):
        addr, cmd = fr.addr, fr.cmd

        if not present and cmd != CMD_SEARCH:
            return build_frame(addr, cmd, bytes([0x01]))

        if cmd == CMD_SEARCH:
            return build_frame(addr, cmd, bytes([0x00 if present else 0x01]))

        if cmd == CMD_ANTICOLL:
            return build_frame(addr, cmd, bytes([0x00]) + self.uid)

        if cmd == CMD_SELECT:
            return build_frame(addr, cmd, bytes([0x00, S50_TYPE_BYTE]))

        if cmd == CMD_AUTH:
            blk = fr.data[1]
            self.authed.update(range((blk // 4) * 4, (blk // 4) * 4 + 4))
            return build_frame(addr, cmd, bytes([0x00]))

        if cmd == CMD_READ:
            blk = fr.data[0]
            if blk not in self.authed:
                return build_frame(addr, cmd, bytes([0x02]))
            return build_frame(addr, cmd, bytes([0x00]) + bytes(self.blocks[blk]))

        if cmd == CMD_WRITE:
            blk = fr.data[0]
            if blk not in self.authed:
                return build_frame(addr, cmd, bytes([0x02]))
            self.blocks[blk][:] = fr.data[1:17]
            return build_frame(addr, cmd, bytes([0x00]))

        return build_frame(addr, cmd, bytes([0x7F]))


def read_frames(ser):
    buf = bytearray()
    in_frame = False
    while True:
        b = ser.read(1)
        if not b:
            continue
        v = b[0]
        if not in_frame:
            if v == START:
                buf.clear()
                buf.append(v)
                in_frame = True
        else:
            buf.append(v)
            if v == STOP:
                yield bytes(buf)
                in_frame = False


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    gate = ActiveSwipeGate(5.0)
    threading.Thread(target=console_listener, args=(gate,), daemon=True).start()

    card = S50CardEmu()
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    print("[EMU] Opened", ser.port)

    for raw in read_frames(ser):
        fr = parse_frame(raw)
        if not fr.valid:
            continue
        present = gate.is_present()
        resp = card.handle(fr, present)
        ser.write(resp)
        ser.flush()


if __name__ == "__main__":
    main()
