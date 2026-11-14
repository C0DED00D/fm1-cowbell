#!/usr/bin/env python3
"""Send COWBELL's sample bank (out/bank.bin, from tools/make_samples.py) into an FM-1 running the COWBELL
firmware, over USB-MIDI. The bank lands in the FM-1's flash and stays there through power-offs and
firmware updates of COWBELL.

  cowbell_upload.py [BANK] [--port NAME]      upload (asks nothing; ~1 minute for 230 KB)
  cowbell_upload.py --info [--port NAME]      what the FM-1 has
  cowbell_upload.py [BANK] --emu-script FILE  the same messages as an fm1-emu FM1_USB_SEND script (testing)

Needs mido and python-rtmidi. Close other programs that use the FM-1's MIDI port first (Windows lets only
one have it). The protocol is in fw/app/cowbell_host.c."""
import argparse
import os
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEAD = [0xF0, 0x7D, 0x43, 0x42]
CHUNK = 512
STATUS = {1: "refused (size or order)", 2: "flash erase failed", 3: "flash write failed",
          11: "no bank header", 12: "bad size", 13: "CRC mismatch", 14: "bad sample entry", 19: "incomplete"}


def pack7(data):
    out = []
    for i in range(0, len(data), 7):
        group = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << k for k, b in enumerate(group)))
        out += [b & 0x7F for b in group]
    return out


def unpack7(data):
    out, i = [], 0
    while i < len(data):
        hi = data[i]
        i += 1
        for k in range(7):
            if i >= len(data):
                break
            out.append(data[i] | (((hi >> k) & 1) << 7))
            i += 1
    return bytes(out)


def msg(cmd, payload=b""):
    return HEAD + [cmd] + pack7(payload) + [0xF7]


def messages(bank):
    yield 0x02, msg(0x02, struct.pack("<I", len(bank)))
    for off in range(0, len(bank), CHUNK):
        yield 0x03, msg(0x03, struct.pack("<I", off) + bank[off:off + CHUNK])
    yield 0x04, msg(0x04)


class Link:
    def __init__(self, port):
        import mido
        self.mido = mido
        keys = [port.lower()] if port else ["fm-1", "usb-midi"]   # the SDK's USB name is "USB-Midi"
        ins = [n for n in mido.get_input_names() if any(k in n.lower() for k in keys)]
        outs = [n for n in mido.get_output_names() if any(k in n.lower() for k in keys)]
        if not ins or not outs:
            sys.exit(f"FM-1 not found (MIDI inputs: {mido.get_input_names()}). Plug it in, close other MIDI "
                     "programs, or name the port with --port.")
        self.inp = mido.open_input(ins[0])
        self.out = mido.open_output(outs[0])
        time.sleep(0.3)
        self.name = ins[0]

    def ask(self, m, cmd, timeout=3.0):
        """send m, wait for the reply to cmd: (status, values)"""
        for _ in self.inp.iter_pending():
            pass
        self.out.send(self.mido.Message("sysex", data=m[1:-1]))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            r = self.inp.poll()
            if r is None:
                time.sleep(0.001)
                continue
            if r.type != "sysex":
                continue
            d = list(r.data)
            if d[:3] == HEAD[1:] and len(d) >= 5 and d[3] == (0x40 | cmd):
                raw = unpack7(d[5:])
                return d[4], [struct.unpack_from("<I", raw, 4 * i)[0] for i in range(len(raw) // 4)]
        return None, []


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bank", nargs="?", default=os.path.join(ROOT, "out", "bank.bin"))
    ap.add_argument("--port", help="part of the FM-1's MIDI port name (default: FM-1 or USB-Midi)")
    ap.add_argument("--info", action="store_true", help="only ask what the FM-1 has")
    ap.add_argument("--emu-script", metavar="FILE", help="write an FM1_USB_SEND script instead of sending")
    ap.add_argument("--start", type=int, default=3000, help="--emu-script: the first message's time, ms")
    ap.add_argument("--gap", type=int, default=12, help="--emu-script: ms between messages")
    ap.add_argument("--din", action="store_true", help="--emu-script: as FM1_BENCH_KEYS through the DIN jack "
                    "(fm1-emu's USB does not enumerate M-VAVE's SDK); give --gap 250")
    a = ap.parse_args()

    if a.emu_script:
        bank = open(a.bank, "rb").read()
        mk = (lambda t, m: f"{t}:m" + bytes(m).hex()) if a.din else (lambda t, m: f"{t}:" + bytes(m).hex())
        t, parts = a.start, [mk(a.start - 500, msg(0x01))]
        for _, m in messages(bank):
            parts.append(mk(t, m))
            t += a.gap
        parts.append(mk(t + 500, msg(0x01)))
        with open(a.emu_script, "w") as f:
            f.write(("," if a.din else ";").join(parts))
        print(f"{a.emu_script}: {len(parts)} messages, the last at {t + 500} ms")
        return

    link = Link(a.port)
    st, v = link.ask(msg(0x01), 0x01)
    if st is None:
        sys.exit(f"{link.name} did not answer: is the FM-1 running the COWBELL firmware?")
    print(f"{link.name}: COWBELL firmware v{v[2]}, {v[0]} samples, room for {v[1]} bytes")
    if a.info:
        return
    bank = open(a.bank, "rb").read()
    if len(bank) > v[1]:
        sys.exit(f"{a.bank} is {len(bank)} bytes, the FM-1 has room for {v[1]}")
    n = (len(bank) + CHUNK - 1) // CHUNK
    t0 = time.monotonic()
    for i, (cmd, m) in enumerate(messages(bank)):
        for attempt in range(3):
            st, v = link.ask(m, cmd, timeout=5.0)
            if st is not None:
                break
        if st is None:
            sys.exit("\nthe FM-1 stopped answering: replug it and run this again (the bank is resent whole)")
        if st:
            sys.exit(f"\nthe FM-1 refused it: {STATUS.get(st, st)}")
        if cmd == 0x03 and (i % 16 == 0 or i == n):
            print(f"\rsending {100 * i // n:3d}%", end="", flush=True)
    print(f"\rdone: {v[0] if v else '?'} samples in the FM-1, {len(bank)} bytes in {time.monotonic() - t0:.0f} s")


if __name__ == "__main__":
    main()
