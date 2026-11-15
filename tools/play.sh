#!/bin/bash
# Get COWBELL ready for play.cmd (which runs the emulator natively on Windows):
#   out/play_flash.bin  the flash with COWBELL in it and picked, so fm1-dock starts it at boot
#   emu/fm1-live.exe    the emulator, cross-compiled for Windows (once), and emu/fm1-dock.fwsc
# "play.sh fresh" starts again from the last build (forgets saved patterns); a new build does it too.
# Your patterns live in out/play_flash.bin between runs.
set -e
PROJ=$(cd "$(dirname "$0")/.." && pwd)
DOCK=$HOME/work/fm1-dock
EMU=$HOME/work/fm1-emu
FWSC="$DOCK/docs/fm1-dock-0.7.fwsc"
FLASH=$PROJ/out/play_flash.bin
EXE=$EMU/target/x86_64-pc-windows-gnu/release/fm1-live.exe

mkdir -p "$PROJ/emu"
if [ ! -f "$EXE" ]; then
    echo "building the Windows emulator (once, ~1 min) ..."
    source ~/.cargo/env
    (cd "$EMU" && CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc \
        cargo build -q --release -p fm1-live --target x86_64-pc-windows-gnu)
fi
[ "$PROJ/emu/fm1-live.exe" -nt "$EXE" ] || cp "$EXE" "$PROJ/emu/fm1-live.exe"
[ -f "$PROJ/emu/fm1-dock.fwsc" ] || cp "$FWSC" "$PROJ/emu/fm1-dock.fwsc"

if [ "$1" = fresh ] || [ ! -f "$FLASH" ] || [ "$PROJ/out/flash.bin" -nt "$FLASH" ]; then
    echo "preparing: COWBELL picked so it starts at boot ..."
    cp "$PROJ/out/flash.bin" /tmp/cowbell_play.bin
    (cd "$DOCK" && python3 tools/dock.py emu --fwsc "$FWSC" --flash /tmp/cowbell_play.bin --ms 7000 \
        --keys "$(cat "$PROJ/out/select.txt")" --out /tmp/cowbell_prep >/dev/null)
    cp /tmp/cowbell_play.bin "$FLASH"
fi
