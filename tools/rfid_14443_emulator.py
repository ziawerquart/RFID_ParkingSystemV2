# Windows host: Python opens COMx (one end of com0com pair), VMware guest uses the other end.
# Emulates a "reader+card" for the Qt project (commands 0x46~0x4C), S50, KeyA(0x60).

import serial
import time
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

S50_TYPE_BYTE = 0x08  # your Qt code treats 0x08 as S50

DEFAULT_UID = bytes.fromhex("B7D2FF79")  # 4 bytes
DEFAULT_KEYA = bytes.fromhex("FFFFFFFFFFFF")  # 6 bytes


def u8(x):
    return x & 0xFF


def checksum(addr_le2, length, cmd, data):
    """
    Matches IEEE1443Package.cpp: sum of addr low/high + len + cmd + data bytes (uint8 overflow)
    """
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
    """
    raw: a complete RAW frame with START/STOP, possibly escaped in-between.
    Returns Frame(valid=False) if parsing fails.
    """
    if len(raw) < 1 or raw[0] != START or raw[-1] != STOP:
        return Frame(0, 0, b"", valid=False)

    content_escaped = raw[1:-1]
    content = unescape_content(content_escaped)

    # content = addr(2 LE) + len(1) + cmd(1) + data(...) + chksum(1)
    if len(content) < 2 + 1 + 1 + 1:
        return Frame(0, 0, b"", valid=False)

    addr_le2 = content[0:2]
    length = content[2]
    cmd = content[3]

    # The project's constructor logic is quirky about len including STOP,
    # but in practice: data_len = length - 3 (cmd + chksum + STOP)
    data_len = length - 3
    if data_len < 0:
        return Frame(0, 0, b"", valid=False)

    # Now ensure content has enough bytes for data + chksum
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
    # Match IEEE1443Package.cpp: len = data_size + 1(cmd) + 1(chksum) + 1(STOP)
    length = u8(len(data) + 3)
    ch = checksum(addr_le2, length, cmd, data)

    content = addr_le2 + bytes([length, cmd]) + data + bytes([ch])
    raw = bytes([START]) + escape_content(content) + bytes([STOP])
    return raw


class S50CardEmu:
    def __init__(self, uid=DEFAULT_UID, keya=DEFAULT_KEYA):
        self.uid = uid
        self.keya = keya

        # S50: blocks 0..63, each 16 bytes
        self.blocks = {}
        for i in range(64):
            self.blocks[i] = bytearray(16)
        # Put something recognizable in block 1/2 for demo
        self.blocks[1][:] = b"HELLO_RFID_BLOCK1"[0:16]
        self.blocks[2][:] = b"DEMO_DATA_BLOCK2!"[0:16]
        # Track auth status per block (simple)
        self.authed_blocks = set()

    def handle(self, fr):
        addr = fr.addr  # respond to same addr
        cmd = fr.cmd

        # Default: status=0(success)
        if cmd == CMD_SEARCH:
            # Return only status byte
            return build_frame(addr, cmd, bytes([0x00]))

        if cmd == CMD_ANTICOLL:
            # data[0] is bcnt (expected 0x04)
            if len(fr.data) < 1 or fr.data[0] != 0x04:
                return build_frame(addr, cmd, bytes([0x01]))  # fail
            return build_frame(addr, cmd, bytes([0x00]) + self.uid)

        if cmd == CMD_SELECT:
            # expects 4-byte uid
            if len(fr.data) != 4 or fr.data != self.uid:
                return build_frame(addr, cmd, bytes([0x01]))
            # status + type byte(0x08 for S50)
            return build_frame(addr, cmd, bytes([0x00, S50_TYPE_BYTE]))

        if cmd == CMD_AUTH:
            # expects: 0x60 + block(1) + key(6)
            if len(fr.data) != 8:
                return build_frame(addr, cmd, bytes([0x01]))
            mode = fr.data[0]
            block = fr.data[1]
            key = fr.data[2:8]
            if mode != 0x60:  # KeyA only
                return build_frame(addr, cmd, bytes([0x01]))
            if key != self.keya:
                return build_frame(addr, cmd, bytes([0x02]))  # auth fail
            if block >= 64:
                return build_frame(addr, cmd, bytes([0x03]))  # invalid block
            sector = self.sector_of_block(block)
            for b in self.blocks_in_sector(sector):
                self.authed_blocks.add(b)
            return build_frame(addr, cmd, bytes([0x00]))

        if cmd == CMD_READ:
            # expects: block(1)
            if len(fr.data) != 1:
                return build_frame(addr, cmd, bytes([0x01]))
            block = fr.data[0]
            if block >= 64:
                return build_frame(addr, cmd, bytes([0x03]))
            # optional: require auth
            if block not in self.authed_blocks:
                return build_frame(addr, cmd, bytes([0x02]))  # not authed
            return build_frame(addr, cmd, bytes([0x00]) + bytes(self.blocks[block][:16]))

        if cmd == CMD_WRITE:
            # expects: block(1) + 16 bytes
            if len(fr.data) != 17:
                return build_frame(addr, cmd, bytes([0x01]))
            block = fr.data[0]
            payload = fr.data[1:17]
            if block >= 64:
                return build_frame(addr, cmd, bytes([0x03]))
            if block not in self.authed_blocks:
                return build_frame(addr, cmd, bytes([0x02]))  # not authed
            self.blocks[block][0:16] = payload[0:16]
            return build_frame(addr, cmd, bytes([0x00]))

        # Unknown cmd
        return build_frame(addr, cmd, bytes([0x7F]))

    def sector_of_block(self, block):
        # S50: 16 sectors, 4 blocks per sector
        return block // 4

    def blocks_in_sector(self, sector):
        base = sector * 4
        return range(base, base + 4)


def read_frames(ser):
    """
    Generator: yields complete RAW frames by scanning START..STOP.
    """
    buf = bytearray()
    in_frame = False
    while True:
        b = ser.read(1)
        if not b:
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


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9", help="Windows COM port (one end of com0com), e.g. COM9")
    ap.add_argument("--baud", type=int, default=115200, help="Baud rate (not critical for virtual ports)")
    ap.add_argument("--uid", default=DEFAULT_UID.hex(), help="4-byte UID hex, e.g. b7d2ff79")
    ap.add_argument("--keya", default=DEFAULT_KEYA.hex(), help="6-byte KeyA hex, default ffffffffffff")
    args = ap.parse_args()

    card = S50CardEmu(uid=bytes.fromhex(args.uid), keya=bytes.fromhex(args.keya))

    # pyserial on Windows uses "COMx"
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    print("[EMU] Opened %s @ %d" % (ser.port, args.baud))
    print("[EMU] UID=%s  KeyA=%s  Type=S50" % (card.uid.hex(), card.keya.hex()))

    try:
        for raw in read_frames(ser):
            fr = parse_frame(raw)
            if not fr.valid:
                print("[EMU] RX invalid frame: %s" % raw.hex())
                continue
            print("[EMU] RX cmd=0x%02X data=%s" % (fr.cmd, fr.data.hex()))

            resp = card.handle(fr)
            ser.write(resp)
            ser.flush()
            # tiny delay to mimic device response time
            time.sleep(0.01)
            print("[EMU] TX %s" % resp.hex())
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("[EMU] Closed.")


if __name__ == "__main__":
    main()
