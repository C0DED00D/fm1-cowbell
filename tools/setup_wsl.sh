#!/bin/bash
# One-time setup of the FM-1 dev environment inside WSL Ubuntu (run from this folder in Windows).
#   wsl -d Ubuntu -u root -- bash tools/setup_wsl.sh root
#   wsl -d Ubuntu          -- bash tools/setup_wsl.sh user
# Gives: ~/.jieli/toolchain (JieLi clang for the chip), ~/fw-AC79_AIoT_SDK (three files fm1-dock needs),
# ~/work/fm1-dock (the host + module tools), ~/work/fm1-emu/target/release/fm1-live (the emulator).
set -e
case "$1" in
root)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q build-essential pkg-config curl git xz-utils \
        libasound2-dev libx11-dev libxkbcommon-dev libxcursor-dev libxrandr-dev libxi-dev libwayland-dev \
        python3-pil python3-mido python3-rtmidi python3-numpy
    ;;
user)
    mkdir -p ~/work
    cd ~/work
    [ -d fm1-dock ] || git clone --depth 1 https://github.com/joakimeriksson/fm1-dock.git
    [ -d fm1-emu ]  || git clone --depth 1 https://github.com/joakimeriksson/fm1-emu.git
    [ -x ~/.jieli/toolchain/pi32v2/bin/clang ] || sh fm1-dock/tools/get_toolchain.sh
    [ -d ~/fw-AC79_AIoT_SDK ] || git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
        https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
    command -v cargo >/dev/null || [ -x ~/.cargo/bin/cargo ] || \
        curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
    . ~/.cargo/env
    cd ~/work/fm1-emu && cargo build --release -p fm1-live -p fm1-emu
    echo "setup done"
    ;;
*) echo "usage: setup_wsl.sh root|user"; exit 1 ;;
esac
