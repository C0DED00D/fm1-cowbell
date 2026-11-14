"""A flash image for the emulator: the current COWBELL firmware (fw/build/COWBELL.fwsc) with out/bank.bin
already in its user partition (flash 0x7C000; the bank at +0x5000), as cowbell_upload.py leaves a real FM-1.
  python tools/emu_flash.py OUT.bin"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "fw_play_flash.bin"))
if os.path.exists(out):
    os.remove(out)
subprocess.run([os.path.join(ROOT, "emu", "fm1-live.exe"), "--fwsc", os.path.join(ROOT, "fw", "build", "COWBELL.fwsc"),
                "--flash", out, "--bench", "1500"], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
d = bytearray(open(out, "rb").read())
b = open(os.path.join(ROOT, "out", "bank.bin"), "rb").read()
at = 0x7C000 + 0x5000
d[at:at + len(b)] = b
open(out, "wb").write(d)
print(f"{out}: the firmware, and the bank ({len(b)} bytes) at {at:#x}")
