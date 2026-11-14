# Licensing

COWBELL is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
[LICENSE](LICENSE)). If you give someone a firmware built from it, give them its source under the same licence too.

## Who made what

| Files | From | Licence |
| --- | --- | --- |
| `cowbell/`, `fw/app/cowbell_host.c`, `fw/app/cowbell/`, `tools/`, `docs/` | COWBELL | GPL-3.0-only |
| `fw/app/` files marked `GPL-3.0-only WITH GPL-3.0-linking-exception` (`board/`, `synth/`, `audio_output.*`, `fm_patch.h`, `instrument.h`, `tuning_hook.h`, `version.h`) | M-VAVE's [fm1_open](https://github.com/m-vave/fm1_open) app, Copyright (c) 2026 珠海笙科智能科技有限公司 (M-VAVE) | GPL-3.0-only with fm1_open's linking permission: [LICENSES/fm1_open-LICENSE.md](LICENSES/fm1_open-LICENSE.md) |
| `fw/app/hb_module.h` | [fm1-dock](https://github.com/joakimeriksson/fm1-dock)'s module interface | GPL-3.0-only |
| `fw/app/felucca_font.h` | the Terminus font (ter-u16n), as rendered by fm1-dock | SIL OFL 1.1: [LICENSES/Terminus-LICENSE.txt](LICENSES/Terminus-LICENSE.txt) |
| `docs/bank.bin`, `fw/app/cowbell/starter.c` | cowbell samples recorded for COWBELL from the maker's own Roland TR-8S and Alesis DM-10 | GPL-3.0-only |

The firmware links with fm1_open's binary SDK (`sdk/lib/libfm1_engine.a` and its components), which is not in
this repository and keeps its own terms: see [LICENSES/fm1_open-THIRD_PARTY.md](LICENSES/fm1_open-THIRD_PARTY.md).
fm1_open's licence permits conveying COWBELL linked with it.

"M-VAVE" and "FM-1" are names of M-VAVE. COWBELL is not made or endorsed by M-VAVE or Korg.
