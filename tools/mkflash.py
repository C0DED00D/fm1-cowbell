#!/usr/bin/env python3
"""Lay a module's deploy into a flash image, as fm1-dock's deploy would leave it (fm1-dock's tools/tryit.py),
and print the input script that picks it at boot.

  mkflash.py FM1DOCK_DIR BASE_FLASH MODULE.dock OUT_FLASH     -> prints the SELECT script entry
Saves ~100 s of emulated SysEx through the DIN jack a build."""
import sys
from pathlib import Path

dock_dir, base, mod, out = (Path(a) for a in sys.argv[1:5])
sys.path.insert(0, str(dock_dir / "tools"))
import hbmod  # noqa: E402
import tryit  # noqa: E402

flash = bytearray(base.read_bytes())
blob = mod.read_bytes()
for at, data in tryit.sectors_for(blob).items():
    if at + len(data) > len(flash):
        raise SystemExit(f"flash image is {len(flash)} B: no room at {at:#x}")
    flash[at:at + len(data)] = data
out.write_bytes(flash)
print(tryit.script(hbmod.parse(blob)[0]["name"]).strip())
