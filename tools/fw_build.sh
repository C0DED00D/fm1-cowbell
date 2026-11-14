#!/bin/bash
# Build the FM-1 firmware in fw/ (an fm1_open app) with JieLi's Linux toolchain, in WSL:
#   bash tools/fw_build.sh [APPDIR]            (in WSL / Linux; FM1_OPEN: fm1_open's checkout, default third_party/fm1_open)
# Mirrors fm1_open's sdk/Build.ps1 (compile, build identity, link); the SDK itself is used from
# third_party/fm1_open/sdk unchanged. Then tools/fw_package.ps1 (Windows) makes the .fwsc.
set -e
ulimit -n 8192
PROJ=$(cd "$(dirname "$0")/.." && pwd)
SDK=${FM1_OPEN:-$PROJ/third_party/fm1_open}/sdk
[ -f "$SDK/manifest.json" ] || { echo "no fm1_open SDK at $SDK (set FM1_OPEN)"; exit 1; }
APP=${1:-$PROJ/fw/app}
BUILD=$PROJ/fw/build
TC=$HOME/.jieli/toolchain/pi32v2/bin
rm -rf "$BUILD/obj"; mkdir -p "$BUILD/obj" "$BUILD/generated"
# COWBELL's own sources: one copy, shared with the fm1-dock module (fw/app/cowbell/samples.h is the FM-1's own)
mkdir -p "$APP/cowbell"
cp "$PROJ/cowbell/cowbell.c" "$PROJ/cowbell/art.c" "$PROJ/cowbell/art.h" "$APP/cowbell/"
FW_NO_LTO="${FW_NO_LTO:-} cowbell_host.c cowbell.c"
cd "$BUILD"
FLAGS=(${FW_CFLAGS:-} -target pi32v2 -integrated-as -mcpu=r3 -mfprev1 -Wuninitialized -Wno-invalid-noreturn -fno-common -flto
  -fallow-pointer-null -fprefer-gnu-section -femulated-tls -Wno-shift-negative-value -Wframe-larger-than=2560
  -fms-extensions -fno-unwind-tables -ffunction-sections -fdata-sections -fmessage-length=0
  -cl-single-precision-constant -mllvm -pi32v2-large-program=true -D__GCC_PI32V2__ -O2)
for d in "$SDK/include" "$SDK/include/libc" "$APP/synth" "$SDK/config" "$APP" "$APP/board"; do FLAGS+=("-I$d"); done
# per-file options, as Build.ps1 (NO_LTO: entry units the LTO plugin could drop; OZ: control-only code)
NO_LTO=" screen.c transfer.c midi_router.c ${FW_NO_LTO:-} "
OZ=" project.c transfer.c samples.c screen.c presets.c midi_router.c fm_factory.c fm_library.c ${FW_OZ:-} "
OBJS=(); i=0
for src in $(cd "$APP" && find . -name '*.c' -o -name '*.S' | grep -v '^./synth/' | sed 's|^\./||' | LC_ALL=C sort); do
    base=$(basename "$src"); opt=()
    [[ "$NO_LTO" == *" $base "* ]] && opt+=(-fno-lto)
    [ "$src" = board/panel.c ] && opt+=(-fno-lto -fno-pic)
    opt+=(-fwrapv -fno-fast-math -ffp-contract=off)
    [[ "$OZ" == *" $base "* ]] && opt+=(-Oz)
    [ "$base" = screen.c ] && opt+=(-fno-inline-functions)
    out="obj/${i}_$(echo "$src" | tr -c 'a-zA-Z0-9._\n-' _).obj"; i=$((i+1))
    echo "compile $src"
    "$TC/clang" "${FLAGS[@]}" "${opt[@]}" -c "$APP/$src" -o "$out"
    OBJS+=("$out")
done
PRODUCT=$(sed -n 's/^#define FM1_UPDATE_PRODUCT "\(FM-1_8[0-9]\{6\}\)".*/\1/p' "$APP/version.h")
[ -n "$PRODUCT" ] || { echo "app/version.h: no FM1_UPDATE_PRODUCT"; exit 1; }
ID=$( (echo dx7; sha256sum "$SDK/manifest.json" | cut -c1-64; cd "$APP" && find . -type f | LC_ALL=C sort | while read f; do echo "app/${f#./}:$(sha256sum "$f" | cut -c1-64)"; done) | sha256sum | cut -c1-64)
IDB=$(echo "$ID" | sed 's/../0x&,/g; s/,$//')
cat > generated/build_identity.c <<EOC
#include "layout.h"
const struct fm1_firmware_layout fm1_layout = {536576U,7U};
const unsigned char fm1_build_id[32] = {$IDB};
const unsigned char fm1_update_identity[27] = "$PRODUCT";
EOC
"$TC/clang" "${FLAGS[@]}" -c generated/build_identity.c -o obj/build_identity.obj
OBJS+=(obj/build_identity.obj)
sed -e 's/@SBC_DECODER@/sbc_decoder\n/' -e 's/@ACL_TX_EXTRA@/ + 1024/' -e 's/@ACL_RX_EXTRA@/ + 4096/' "$SDK/config/linker.ld.in" > generated/linker
echo "link ($PRODUCT)"
"$TC/ld" --plugin "$TC/LLVMgold.so" -o sdk.elf "${OBJS[@]}" \
  -flto --plugin-opt=-pi32v2-always-use-itblock=false --plugin-opt=-enable-ipra=true \
  --plugin-opt=-pi32v2-merge-max-offset=4096 --plugin-opt=-pi32v2-enable-simd=true --plugin-opt=mcpu=r3 \
  --plugin-opt=-global-merge-on-const --plugin-opt=-inline-threshold=5 --plugin-opt=-inline-max-allocated-size=32 \
  --plugin-opt=-inline-normal-into-special-section=true --plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar \
  --plugin-opt=-pi32v2-enable-rep-memop --plugin-opt=-emulated-tls --sort-common \
  --plugin-opt=-pi32v2-large-program=true --plugin-opt=-pi32v2-csync-before-rti=true --start-group \
  --whole-archive "$SDK/lib/libfm1_engine.a" --no-whole-archive "$SDK/lib/libc.a" "$SDK/lib/libcompiler_rt.a" \
  --end-group --gc-sections -Tgenerated/linker -M=sdk.map --undefined=connectivity_wl82_bluetooth_start \
  --undefined=audio_app_bluetooth_open --undefined=bt_get_local_name --undefined=bt_get_mac_addr > link.log 2>&1 \
  || { tail -30 link.log; exit 1; }
"$TC/nm" --print-size sdk.elf > sdk.syms
for s in text data ram0_data cache_ram_data; do "$TC/objcopy" -O binary -j ".$s" sdk.elf "generated/$s.bin"; done
echo "$PRODUCT" > generated/product.txt
ls -la generated/*.bin
