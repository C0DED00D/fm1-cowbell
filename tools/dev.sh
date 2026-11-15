#!/bin/bash
# The dev loop, in WSL Ubuntu. From Windows (from this folder; Git Bash: MSYS_NO_PATHCONV=1 first):
#   wsl -d Ubuntu -- bash tools/dev.sh build
#       compile the module, boot fm1-dock 0.7 in fm1-emu, deploy COWBELL through the DIN jack (slow: ~90 s
#       of emulated MIDI) and keep the flash in out/flash.bin, COWBELL in it and picked
#   wsl -d Ubuntu -- bash tools/dev.sh run [MS] [KEYS] [NAME]
#       boot from a copy of out/flash.bin (COWBELL starts at once), play KEYS for MS ms; out/NAME/ gets
#       audio.wav, panel.png (the screen at the end), log.txt
# KEYS (fm1-dock's tools/dock.py): comma-separated `ms:id` press (id 14+k for note key k, 0 = F3 .. 26 = G5;
#   buttons 0 OCT- 1 OCT+ 2 FX 3 SEL 4 ENV 5 LFO 6 EDIT 7 GLO 8 HOME 9 SAVE 10 ARP 11 SEQ 12 PLAY 13 REC),
#   `ms:id^` release, `ms:eN:clicks` encoder N (0 SELECT, 1 ALGORITHM, 2..5 KNOB1..4, 6 PRESETS), `ms:mHEX` MIDI.
set -e
PROJ=$(cd "$(dirname "$0")/.." && pwd)
DOCK=$HOME/work/fm1-dock
FWSC="${FWSC:-$DOCK/docs/fm1-dock-0.7.fwsc}"
export JIELI_TOOLCHAIN=$HOME/.jieli/toolchain AC79_SDK=$HOME/fw-AC79_AIoT_SDK
export FM1_LIVE=$HOME/work/fm1-emu/target/release/fm1-live
mkdir -p "$PROJ/out"

case "${1:-build}" in
build)
    rm -rf "$DOCK/modules/cowbell"
    cp -r "$PROJ/cowbell" "$DOCK/modules/cowbell"
    cd "$DOCK"
    python3 tools/hbmod.py modules/cowbell
    python3 tools/hbmod.py --info build/modules/cowbell.dock
    cp build/modules/cowbell.dock "$PROJ/out/"
    BASE=$HOME/work/fm1dock_fresh_flash.bin
    if [ ! -f "$BASE" ]; then                   # fm1-dock's flash after a first boot, nothing deployed
        python3 tools/dock.py emu --fwsc "$FWSC" --ms 2000 --flash "$BASE" --out /tmp/fm1_fresh >/dev/null
    fi
    python3 "$PROJ/tools/mkflash.py" "$DOCK" "$BASE" build/modules/cowbell.dock "$PROJ/out/flash.bin" \
        > "$PROJ/out/select.txt"
    echo "built: out/cowbell.dock, out/flash.bin (COWBELL deployed), out/select.txt (picks it at boot)"
    ;;
run)
    NAME="${4:-run}"
    cp "$PROJ/out/flash.bin" /tmp/cowbell_flash.bin
    KEYS="$(cat "$PROJ/out/select.txt")${3:+,$3}"
    cd "$DOCK"
    python3 tools/dock.py emu --fwsc "$FWSC" --flash /tmp/cowbell_flash.bin --ms "${2:-6000}" \
        --keys "$KEYS" --out "$PROJ/out/$NAME"
    echo "out/$NAME: audio.wav panel.png log.txt"
    ;;
*) echo "usage: dev.sh build | run [MS] [KEYS] [NAME]"; exit 1 ;;
esac
