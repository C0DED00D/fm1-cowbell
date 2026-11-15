# COWBELL: a sample cowbell groovebox for the FM-1

A homage to the volca cowbell (the April-fools volca with FEVER, PRESCRIPTION and the big MORE knob),
built as an **fm1-dock module**: a C plug-in that fm1-dock loads from flash (hold HOME, pick COWBELL).
It is a step sequencer with 8 cowbell parts. Each part plays one sample from a built-in bank, pitch-shifted,
and the MORE knob sets the **ratchet** (repeats inside a step).

## Why fm1-dock and not a whole firmware

| | fm1-dock module | whole firmware (M-VAVE `fm1_open` or Felucca-style) |
| --- | --- | --- |
| Install | `.dock` file, deployed in seconds over USB, beside the other synths | reflash the whole FM-1 |
| Risk | the host checks it, falls back to a sine if it crashes | a bad build can brick until rescued |
| Emulator | `tools/dock.py emu` runs it headless: WAV, screen PNG, log, CPU load | yes, but no module tooling |
| Cost | 272 KiB RAM, image ≤ 272 KiB, shares flash with other modules | whole chip |

**Decided: COWBELL is the only added module and takes the whole module area** (296 KiB, the module itself up
to 272 KiB) and the free flash banks. fm1-dock's built-in synths (CS-1, Scintilla, OberVave, SINE) live in
the host's own flash and stay; nothing else can be added beside COWBELL.

fm1-dock gives the whole panel and screen to a module that asks for it (`HB_F_PANEL | HB_F_SCREEN`), which a
groovebox needs. Holding HOME for 1.5 s still opens the synth picker.

## The panel mapping

The FM-1's layout lines up with the volca's:

```
 FM-1                                   volca cowbell
 ─────────────────────────────────────  ────────────────────────────────────
 SELECT    encoder (left)          ->   FEVER          pitch shift of the part
 PRESETS   encoder (left)          ->   PRESCRIPTION   which sample the part plays
 ALGORITHM encoder (left)          ->   MORE           ratchet (drawn big in the middle of the screen)
 KNOB1                             ->   TEMPO          40..300 BPM, on the red 7-segment readout
 KNOB2                             ->   SWING          50..75 %
 KNOB3                             ->   REVERB MIX
 KNOB4                             ->   DECAY          the part's length (the volca's VOLUME is MASTER)
 MASTER pot                        ->   VOLUME         (the host's)

 PLAY MODE (the default: the keys play, as on M-VAVE's own firmware)
 all 27 keys                       ->   the selected part, chromatically (C4 = its own pitch);
                                        held, a key ratchets at the MORE rate (x1: one hit)
 OCT- / OCT+                       ->   octave (GLO + OCT-/+: the part)

 STEP MODE (SEQ toggles it; the SEQ light is on, the header shows STEP)
 16 white keys, each with its LED  ->   the 16 step buttons and their lights
 black keys 1..8                   ->   PART 1..8: select the part (and play it)
 black keys 9, 10                  ->   ACCENT (held: steps entered are accented), MUTE (FUNC: SOLO)
 black key 11                      ->   MORE! hold for a live roll at the MORE rate
 OCT- / OCT+                       ->   the part

 FX    -> REVERB send of the part    SAVE  -> MEMORY (FUNC+SAVE: WRITE)
 SEL   -> CLEAR the part's steps      ARP   -> STEP MODE / ACTIVE STEP
 ENV   -> MUTE / SOLO                 SEQ   -> STEP mode on / off (play mode)
 LFO   -> MOTION on/off (FUNC: clear) PLAY  -> PLAY / STOP
 EDIT  -> < PART >  with OCT- / OCT+  REC   -> REC (FUNC+REC: STEP REC)
 GLO   -> FUNC (hold: the alternate labels, as the volca's FUNC)
 HOME  -> the host's picker (hold)
```

The encoders are endless, so a "knob position" is a number the module keeps and draws. That suits the
volca's motion sequence: no jumps when a pattern or memory changes.

## How it plays

- **8 parts**, one voice each. A new hit cuts the part's last one with a 1 ms fade (no clicks), as a real
  ratchet does. Eight voices together cost little: no filter per voice, just playback and a gain.
- **Pattern**: 8 parts x 16 steps. Per step: on/off, ratchet (1, 2, 3, 4, 6, 8 hits), and optional locks for
  pitch, decay and accent.
- **MORE = ratchet**. Turning MORE sets the selected part's ratchet. Held step key + MORE locks that step's
  ratchet only. With REC and MOTION on, turning MORE while it plays records the ratchet step by step (the
  volca's motion sequence). The top of the range is "MORE!!": the ratchet ramps up in volume across the step.
  The step's hits are spread evenly over its length, swing included.
- **FEVER = pitch**. -24..+24 semitones, by resampling (faster playback is higher and shorter, as on a volca
  sample or an SP-1200). Held step key + FEVER locks the pitch of that step: cowbell melodies.
- **Play mode (the default)**: all 27 keys play the selected part chromatically (C4 = its own pitch), so the
  FM-1 plays as it does with its own firmware. Hold a key and it ratchets at the MORE rate. With REC and PLAY
  on, played notes go into the steps as pitch locks. **SEQ** switches to step mode for editing steps.
- **PRESCRIPTION** steps through the sample bank: the part plays the new sample at once.
- **Memories**: 10 patterns (the volca's M1..M10), PRESETS+FUNC or SAVE to pick, FUNC+SAVE to write.
- **MIDI**: notes in play the parts (36..43 = parts 1..8; on channel 10 by default) and, in play mode, the
  selected part chromatically. Clock out (F8) while playing so another box can follow.

## Sync

- **Clock out**: F8 24 times a quarter note, FA / FC on PLAY / STOP, over USB and BLE.
- **Clock in over USB** (and BLE, which shares the ring): the host passes F8 / FA / FB / FC to `midi()` in the
  main loop. Each message is timestamped with `ticks()` there and handed to `render()`, which keeps the real
  clock: it estimates the tempo from the last 24 intervals and nudges its own step phase towards the incoming
  one, so main-loop jitter (a screen draw can delay a message by several ms) does not reach the groove.
  FA starts at step 1 on the next tick, FC stops, FB continues; SYNC = AUTO follows when clock arrives.
- **Clock in on the DIN jack**: the host throws realtime bytes away there (dock.c:1100). Letting them through is
  a one-line change in fm1-dock; that needs a pull request to fm1-dock or our own fork of the host.

## The sound engine

- Output 44.1 kHz stereo, rendered in 256-frame blocks (5.8 ms each), done in `render()`.
- **Samples**: mono, 44,100 Hz, 4-bit IMA ADPCM in the module's flash (a quarter of 16-bit's size). Samples
  only ever play forwards from the start (no reverse, no start offset), which is all ADPCM allows and all a
  cowbell needs. Each voice decodes forwards as its 32-bit phase (16.16) advances and interpolates linearly
  between the last two decoded values; the pitch step comes from a table of 2^(n/12), worked out on the
  computer, not on the chip. A retrigger (a new hit, a ratchet) restarts the decoder at the sample's start.
- **The sequencer clock** counts frames inside `render()`, so timing is sample-exact and does not depend on
  the screen or the main loop.
- **Reverb**: a small mono Schroeder reverb (4 combs + 2 allpasses, 16-bit delay lines, ~40 KiB), fed by the
  parts' sends; REVERB MIX mixes it in. Run at 22 kHz to halve its cost.
- **Level**: soft clipping at the end, as eight cowbells can add up.

## The sample bank

**Now: 13 cowbells recorded from the maker's own Roland TR-8S and Alesis DM-10, 143 KiB, at 44.1 kHz**
(`cowbell/samples.c` for the fm1-dock module, `docs/bank.bin` for the firmware; the 22 kHz starter bank compiled
into the firmware is `fw/app/cowbell/starter.c`). `tools/make_samples.py` builds them: see fw/README.md.
The table below is the first, all-synthesized bank (`make_samples.py` without `--wav`), kept as an option:

| # | name | how it is made |
| --- | --- | --- |
| 1 | 808 | two square waves at 540 / 800 Hz, band-passed, two-stage decay (the classic drum-machine cowbell) |
| 2 | 808 LO | the same, an octave down and longer |
| 3 | CR | a CR-78 style: a short square pair with a click |
| 4 | REAL | an acoustic cowbell by modal synthesis: inharmonic partials, a stick strike noise |
| 5 | REAL MUT | the same, damped by a hand |
| 6 | BIG | a big studio cowbell, "MORE" sized: two REAL hits, room, saturation |
| 7 | AGOGO HI | the two-tone agogo, high bell |
| 8 | AGOGO LO | the low bell |
| 9 | TINY | a goat bell |
| 10 | CLANK | a dull iron cowbell |
| 11 | BLOCK | a wood block, for contrast |
| 12 | FM BELL | a 2-operator FM bell (the FM-1's own spirit) |
| 13.. | variants of the above (tighter, longer, struck harder) and your own | |

Budget: ~250 KiB of samples in the module (272 KiB less ~20 KiB of code and art), at 44.1 kHz ADPCM
(~22 KB a second) ~11 s of sound: **~28 cowbells** of ~0.4 s. Plus the free flash banks (8 of 8 KiB, ~64 KiB;
the ninth holds the patterns), loaded into RAM at start: ~7 more, ~35 in all. Short 808-style hits (~0.25 s)
leave room for more; long acoustic rings (~0.8 s) for fewer.

## The screen (240 x 240)

Drawn as the volca's front: the magenta panel and its labels.

```
 ┌──────────────────────────────────────┐
 │ COWBELL   P3 REAL      ▶  ● REC  M1 │  header: part, sample, transport, memory
 │ ┌──────┐   (FEVER)  (PRESCRIPTION)   │
 │ │ 1 2 0│     +7        REAL          │  red 7-segment: tempo, or the value being turned
 │ └──────┘                             │
 │              ╲  │  ╱                 │
 │            ─  ( MORE )  ─            │  the big MORE knob, its pointer, "x4"
 │              ╱  x4  ╲                │
 │ ▢▢▢▢▢▢▢▢ ▢▢▢▢▢▢▢▢   1 2 3 4 5 6 7 8   │  16 steps (lit, playhead); 8 parts (muted dim)
 └──────────────────────────────────────┘
```

Only what changes is redrawn: a full screen is 115 KB over a slow serial link (~77 ms).

### The MORE knob as a picture

- A 96 x 96 knob image (a shaded cap, its skirt and the tick ring), rendered on the computer by
  `tools/make_art.py` and stored with a 16-colour palette: ~4.6 KiB of flash.
- At start it is expanded once into RAM (18 KiB, RGB565). When MORE changes, the base is copied to a scratch
  buffer, the pointer is drawn into it at the new angle (a thick line and a dot, with an edge blend), and the
  96 x 96 block goes out with `blit()`: ~18 KB, ~12 ms over SPI, fine at 30 frames a second.
- Overlaying the pointer instead of storing a filmstrip of turned knobs keeps every angle smooth for a few KiB
  instead of hundreds. FEVER and PRESCRIPTION are smaller pictures done the same way.
- Turning: the ALGORITHM encoder. MORE has 7 positions (x1 x2 x3 x4 x6 x8 MORE!!) over a 270 degree sweep;
  two clicks per position so it does not skip. The pointer glides to the new angle over a few frames.

## Working within the FM-1's limits

| Limit | What it means here |
| --- | --- |
| CPU: 5.8 ms per 256 frames; integer-friendly chip, `float` only single precision, no `double`, no 64-bit division | fixed point throughout; envelopes and ratchet gains once per 16 frames; target under 30 % of a block (fm1-dock stops a module that runs slower than real time) |
| Module RAM: 272 KiB (data + bss), 16 KiB of fast RAM code | reverb ~40 KiB, patterns ~6 KiB, the bank samples ~64 KiB, the knob art 18 KiB; the voice loop can go in fast RAM if it needs to |
| Module flash: image ≤ 272 KiB, and all added modules share 296 KiB | COWBELL takes it all: ~250 KiB of ADPCM samples in the image, ~64 KiB more in the banks |
| Saving: a 240-byte settings record and 8 KiB flash banks; a save stalls the sound ~100 ms | 10 patterns fit one bank; WRITE saves only while stopped; edits also autosave 3 s after the last one, once stopped and silent (as the fm1-dock firmwares do) |
| Screen: 240 x 240 over SPI, canvases up to 240 x 96 | redraw only the parts that changed, at most ~30 times a second |
| Pitch shift by resampling changes the length | as the volca sample does; true time-stretch is not worth the CPU here |
| MIDI clock in: USB-MIDI clock and start/stop/continue reach the module's `midi()` (usb.c:756 -> dock.c:2570), but in the main loop, not the audio interrupt; the DIN jack drops them (dock.c:1100) | follow USB clock with a smoothed tempo estimate (see "Sync"); DIN clock needs a one-line host change, upstream or in a fork |
| 27 keys, 16 white | the keys play (default); in step mode (SEQ) the 16 white keys are the steps, the black keys parts and functions |
| Switching modules forgets RAM | the pattern being edited is lost unless written; edits autosave 3 s after the last one, once stopped and silent: only edits made in the last 3 s, or while playing, can be lost |

## Phase 2

- Your own samples at run time: `tools/dock.py send mybell.wav` (the module's `data()` call, `HB_F_DATA`),
  converted to ADPCM on the computer and kept in the free flash banks (~64 KiB, ~3 s, ~7 cowbells).
- Part-level swing and probability, pattern chaining.
- The second core for the reverb, if the first one gets busy.

## The dev loop

Everything runs in WSL Ubuntu; see `tools/setup_wsl.sh` (one time) and `tools/dev.sh` (every build):
build the `.dock`, boot fm1-dock 0.7 in the fm1-emu emulator, deploy the module, play a scripted set of
presses and knob turns, and write `out/audio.wav`, `out/panel.png` (the screen) and `out/log.txt`
(the module's log, crashes, render load). On a real FM-1: `tools/dock.py deploy out/cowbell.dock`.
