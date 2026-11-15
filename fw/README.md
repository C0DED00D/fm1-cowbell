# COWBELL as the FM-1's own firmware

COWBELL is built on M-VAVE's open SDK, [fm1_open](https://github.com/m-vave/fm1_open), and is installed by
M-VAVE's own update loader, the one every stock update uses.

The synth itself is [`../cowbell/cowbell.c`](../cowbell/cowbell.c) (it also runs as an
[fm1-dock](https://github.com/joakimeriksson/fm1-dock) module). Around it, [`app/cowbell_host.c`](app/cowbell_host.c)
is the host: the screen, LEDs, panel, MIDI and storage on the SDK, and the SDK's application hooks in place of
M-VAVE's groovebox UI. The other files in `app/` come from fm1_open's own app (`board/`, `synth/`, the headers),
plus fm1-dock's module interface (`hb_module.h`) and the Terminus font (`felucca_font.h`).

## Where things live

| | |
| --- | --- |
| Firmware image | 460 KiB of the 1 MiB flash: the SDK's runtime (USB, Bluetooth, the updater), COWBELL and its starter samples |
| Patterns | the user partition's first 16 KiB: two slots, a save goes to the older one, the newer valid one loads |
| Samples | the user partition from +20 KiB: up to 504 KiB, played straight from flash |

The full-quality samples (143 KiB) do not fit in the firmware image: fm1_open's flash layout is fixed, and about
50 KiB of it was free. They are sent once over USB, from the browser at <https://c0ded00d.github.io/fm1-cowbell/>
or with `tools/cowbell_upload.py`, and stay through power-offs and COWBELL updates. Until then COWBELL plays the
starter bank compiled in, [`app/cowbell/starter.c`](app/cowbell/starter.c): the same 13 sounds at 22 kHz, cut at
0.42 s.

Package identity: `FM-1_8038311` ([`app/version.h`](app/version.h)), which reads 0W8ELL with a sideways 3 for
the W.

## Install

Close other programs that use the FM-1's MIDI port, then install `COWBELL.fwsc` from this repo's
[releases](https://github.com/C0DED00D/fm1-cowbell/releases) with any FM-1 installer that takes an fm1_open
package: fm1-dock's `fm1_install.py` (how COWBELL has been installed so far), or
[M-VAVE's workbench](https://open.m-vave.com/fm1-workbench), whose updater accepts fm1_open identities
(`FM-1_8` and six digits; not yet tried with COWBELL):

```bash
python fm1-dock/tools/fm1_install.py COWBELL.fwsc --force
```

(`--force`: the package carries M-VAVE's loader, not Felucca's, which that installer otherwise expects.) COWBELL
starts on its starter samples; then send the full set from the [uploader](https://c0ded00d.github.io/fm1-cowbell/),
or:

```bash
python tools/cowbell_upload.py docs/bank.bin
```

(`pip install mido python-rtmidi` first.)

To go back to M-VAVE's original firmware, use the workbench, which offers it. fm1_open also carries it as
`sdk/firmware/FM-1_015.fwsc` (sha256 `db1642b2…`), but fm1-dock's installer refuses that file before starting:
its pre-check expects a different CRC on the package's user-data file. With that one check skipped, it installs
over COWBELL normally.

Tested on a real FM-1 (one whose flash chip fm1-dock's own loader refuses) on 2026-11-13: V02 installed over COWBELL V01, V15 installed
over V02, and the v2 release installed over V15.

## Build

You need WSL (or Linux) with JieLi's PI32v2 toolchain in `~/.jieli/toolchain` (the Linux build of the clang 4.0.1
that fm1_open's README names), Windows PowerShell for the packer, and fm1_open's SDK:

```bash
git clone https://github.com/m-vave/fm1_open third_party/fm1_open
git -C third_party/fm1_open checkout 4c7496d
```

Then, in WSL from this folder, and in Windows PowerShell:

```bash
bash tools/fw_build.sh
```

```bash
powershell -ExecutionPolicy Bypass -File tools/fw_package.ps1
```

`fw_build.sh` follows fm1_open's `sdk/Build.ps1` with the Linux toolchain; `fw_package.ps1` packs with the SDK's
own `Package.cs`. The result is `fw/build/COWBELL.fwsc`; releases v2 to v4 were built this way. (Set
`FM1_OPEN` to use an fm1_open checkout somewhere else.)

## Samples

`tools/make_samples.py` turns WAV files into the bank: it trims the silence, normalizes, resamples to 44.1 kHz
mono and encodes 4-bit IMA ADPCM, checking each sample's SNR after decoding.

```bash
python tools/make_samples.py --no-builtin --wav YOUR_FOLDER --max-len 1.5
```

writes `out/bank.bin` (send it with the uploader's "Use a different bank file", or `cowbell_upload.py`). With
`--starter --max-len 0.42 --cap-fade 0.1 --min-snr 18` it writes the 22 kHz starter bank,
`fw/app/cowbell/starter.c`, instead. Number the files (`01 808.wav`, ...) to set the order; the name after the
number is what the screen shows, 9 characters at most.

## Notes

On USB the firmware shows up as "USB-Midi" (the SDK's name), not "FM-1"; the upload tool and the page look for
both. No velocity: the SDK reports keys as on / off.

Edits save themselves 3 s after the last one, once stopped and silent (a flash write stalls the sound), as the
fm1-dock firmwares do; SAVE saves at once. Only the last few seconds of edits, or edits while playing, are lost
by a power-off.
