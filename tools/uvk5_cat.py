#!/usr/bin/env python3
"""UV-K5 UART CAT istemcisi (ENABLE_UART_CAT firmware'i icin).

Telsiz normal modda calisirken programlama kablosu uzerinden:
  - sinyal geldiginde / bittiginde olay paketi alir (0x0710 / 0x0711),
  - durum okur (0x0701 -> 0x0702),
  - PTT acar/kapatir (0x0703 -> 0x0704, 2 s watchdog: acik tutmak icin yenile),
  - VFO frekansi ayarlar (0x0705 -> 0x0706).

Kurulum:  pip install pyserial
Ornekler:
  python uvk5_cat.py COM5 listen            # olaylari dinle, sinyal bitince durum oku
  python uvk5_cat.py /dev/ttyUSB0 status
  python uvk5_cat.py COM5 freq 145.500
  python uvk5_cat.py COM5 ptt 3             # 3 saniye TX (amator lisansi gerekir!)
"""

import argparse
import struct
import sys
import time

OBFUSCATION = bytes([0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40,
                     0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80])

FUNCTIONS = ["FOREGROUND", "TRANSMIT", "MONITOR", "INCOMING",
             "RECEIVE", "POWER_SAVE", "BAND_SCOPE"]

RESULTS = {0: "OK", 1: "VFO modunda degil (bellek kanali)", 2: "Mesgul (TX/PTT)",
           3: "Frekans RX araliginin disinda", 4: "Bant degisimi desteklenmiyor",
           5: "Telsiz reddetti"}

BAUD = 38400


# --------------------------------------------------------------------------
# Cerceveleme (firmware app/uart.c ile birebir)
# --------------------------------------------------------------------------

def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def xor_obf(data: bytes) -> bytes:
    return bytes(b ^ OBFUSCATION[i % 16] for i, b in enumerate(data))


def build_frame(cmd_id: int, body: bytes = b"") -> bytes:
    """PC -> telsiz cercevesi: AB CD | len | obf(payload + crc) | DC BA."""
    payload = struct.pack("<HH", cmd_id, len(body)) + body
    crc = struct.pack("<H", crc16_xmodem(payload))
    return (b"\xAB\xCD" + struct.pack("<H", len(payload))
            + xor_obf(payload + crc) + b"\xDC\xBA")


class FrameParser:
    """Telsiz -> PC cercevelerini akistan ayiklar. Yanit CRC tasimaz;
    2 baytlik dolgu obfuscation ile FF FF olmali, buna gore duz/obf mod anlasilir."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        self.buf += data
        out = []
        while True:
            start = self.buf.find(b"\xAB\xCD")
            if start < 0:
                self.buf.clear()
                break
            del self.buf[:start]
            if len(self.buf) < 4:
                break
            size = struct.unpack_from("<H", self.buf, 2)[0]
            total = 4 + size + 4
            if size > 300:
                del self.buf[:2]
                continue
            if len(self.buf) < total:
                break
            raw = bytes(self.buf[4:4 + size + 2])
            tail = bytes(self.buf[4 + size + 2:total])
            del self.buf[:total]
            if tail != b"\xDC\xBA":
                continue
            plain = xor_obf(raw)
            if plain[-2:] == b"\xFF\xFF":
                payload = plain[:-2]
            elif raw[-2:] == b"\xFF\xFF":
                payload = raw[:-2]          # telsiz duz modda
            else:
                continue
            if len(payload) >= 4:
                cmd_id, _ = struct.unpack_from("<HH", payload, 0)
                out.append((cmd_id, payload[4:]))
        return out


# --------------------------------------------------------------------------
# Paket cozumleme
# --------------------------------------------------------------------------

def mhz(f10hz: int) -> str:
    return f"{f10hz / 100000:.5f} MHz"


def rssi_dbm(raw: int) -> float:
    # BK4819 REG_67: yaklasik dBm = raw/2 - 160 (kalibrasyonsuz kaba deger)
    return raw / 2 - 160


def decode(cmd_id: int, d: bytes) -> dict:
    if cmd_id == 0x0702 and len(d) >= 20:
        rx, tx, rssi, bat, fn, rxv, txv, fl, nz, gl, ch, _ = struct.unpack_from("<IIHHBBBBBBBB", d)
        return {"tip": "durum", "rx": mhz(rx), "tx": mhz(tx), "rssi_raw": rssi,
                "rssi_dbm": rssi_dbm(rssi), "batarya_V": bat / 100,
                "fonksiyon": FUNCTIONS[fn] if fn < len(FUNCTIONS) else fn,
                "rx_vfo": rxv, "tx_vfo": txv, "rx_aktif": bool(fl & 1),
                "cat_ptt": bool(fl & 2), "ptt": bool(fl & 4), "olaylar": bool(fl & 8),
                "gurultu": nz, "glitch": gl, "kanal": ch}
    if cmd_id == 0x0704 and len(d) >= 4:
        res, ptt, fn, _ = struct.unpack_from("<BBBB", d)
        return {"tip": "ptt", "sonuc": RESULTS.get(res, res), "cat_ptt": bool(ptt),
                "fonksiyon": FUNCTIONS[fn] if fn < len(FUNCTIONS) else fn}
    if cmd_id == 0x0706 and len(d) >= 8:
        res, f = struct.unpack_from("<B3xI", d)
        return {"tip": "frekans", "sonuc": RESULTS.get(res, res), "frekans": mhz(f)}
    if cmd_id == 0x0710 and len(d) >= 12:
        f, rssi, nz, gl, fn, vfo = struct.unpack_from("<IHBBBB", d)
        return {"tip": "RX_BASLADI", "frekans": mhz(f), "rssi_dbm": rssi_dbm(rssi),
                "gurultu": nz, "glitch": gl, "vfo": vfo,
                "fonksiyon": FUNCTIONS[fn] if fn < len(FUNCTIONS) else fn}
    if cmd_id == 0x0711 and len(d) >= 8:
        dur, peak = struct.unpack_from("<IH", d)
        return {"tip": "RX_BITTI", "sure_s": dur / 100, "tepe_rssi_dbm": rssi_dbm(peak)}
    if cmd_id == 0x0515:
        return {"tip": "versiyon", "versiyon": d[:16].split(b"\0")[0].decode(errors="replace")}
    return {"tip": f"0x{cmd_id:04X}", "ham": d.hex()}


# --------------------------------------------------------------------------
# Istemci
# --------------------------------------------------------------------------

class UVK5Cat:
    def __init__(self, port: str):
        import serial  # pyserial
        self.ser = serial.Serial(port, BAUD, timeout=0.05)
        self.parser = FrameParser()

    def send(self, cmd_id: int, body: bytes = b""):
        self.ser.write(build_frame(cmd_id, body))

    def read_packets(self, timeout: float = 0.3):
        end = time.time() + timeout
        packets = []
        while time.time() < end:
            data = self.ser.read(256)
            if data:
                packets += [decode(i, d) for i, d in self.parser.feed(data)]
            elif packets:
                break
        return packets

    def request(self, cmd_id: int, body: bytes, reply_tip: str, timeout: float = 0.5):
        self.send(cmd_id, body)
        end = time.time() + timeout
        other = []
        while time.time() < end:
            for p in self.read_packets(0.1):
                if p["tip"] == reply_tip:
                    for o in other:
                        print_packet(o)
                    return p
                other.append(p)
        for o in other:
            print_packet(o)
        raise TimeoutError(f"{reply_tip} yaniti gelmedi")

    def status(self):
        return self.request(0x0701, b"", "durum")

    def ptt(self, on: bool):
        return self.request(0x0703, struct.pack("<B3x", 1 if on else 0), "ptt")

    def set_freq(self, mhz_value: float):
        f = int(round(mhz_value * 100000))
        return self.request(0x0705, struct.pack("<I", f), "frekans", timeout=1.0)


def print_packet(p: dict):
    stamp = time.strftime("%H:%M:%S")
    tip = p.pop("tip")
    print(f"[{stamp}] {tip:<11} " + "  ".join(f"{k}={v}" for k, v in p.items()), flush=True)


def main():
    ap = argparse.ArgumentParser(description="UV-K5 UART CAT istemcisi")
    ap.add_argument("port", help="Seri port (COM5, /dev/ttyUSB0 ...)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status", help="Durum oku")
    lp = sub.add_parser("listen", help="RX olaylarini dinle")
    lp.add_argument("--poll", type=float, default=0.0,
                    help="Bosta da N saniyede bir durum oku (0 = sadece sinyal bitiminde)")
    fp = sub.add_parser("freq", help="VFO frekansi ayarla (MHz)")
    fp.add_argument("mhz", type=float)
    pp = sub.add_parser("ptt", help="N saniye TX (watchdog otomatik yenilenir)")
    pp.add_argument("saniye", type=float)
    args = ap.parse_args()

    cat = UVK5Cat(args.port)

    if args.cmd == "status":
        print_packet(cat.status())

    elif args.cmd == "freq":
        print_packet(cat.set_freq(args.mhz))

    elif args.cmd == "ptt":
        print_packet(cat.ptt(True))
        end = time.time() + args.saniye
        try:
            while time.time() < end:
                time.sleep(0.5)
                cat.ptt(True)          # 2 s watchdog'u yenile
        finally:
            print_packet(cat.ptt(False))

    elif args.cmd == "listen":
        print_packet(cat.status())     # olaylari etkinlestirir (ilk 0x07xx komutu)
        last_poll = time.time()
        print("Dinleniyor... (Ctrl+C ile cik)", flush=True)
        try:
            while True:
                for p in cat.read_packets(0.2):
                    tip = p["tip"]
                    print_packet(p)
                    if tip == "RX_BITTI":
                        print_packet(cat.status())   # sinyal bitince periyodik okuma
                        last_poll = time.time()
                if args.poll and time.time() - last_poll >= args.poll:
                    print_packet(cat.status())
                    last_poll = time.time()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    sys.exit(main())
