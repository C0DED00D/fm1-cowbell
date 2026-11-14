# fm1-cowbell

![A pink "volca cowbell" groovebox with a giant MORE knob and FEVER and PRESCRIPTION knobs](docs/cowbell_meme.png)

COWBELL is a cowbell groovebox firmware for the M-VAVE FM-1, built on M-VAVE's open SDK (fm1_open).

## Sample uploader

**https://c0ded00d.github.io/fm1-cowbell/**

The firmware comes with a smaller, 22 kHz copy of its 13 cowbell samples built in, so it plays straight after
installing. The uploader page sends the full-quality set (44.1 kHz, 143 KB) to the FM-1 over USB-MIDI from the
browser (Chrome or Edge); nothing to install. The samples stay through power-offs and firmware updates.

1. Plug the FM-1 in over USB with COWBELL running, and close other programs that use its MIDI port.
2. Open the page, press **Connect to the FM-1**, allow MIDI access.
3. Press **Send to the FM-1** (about ten seconds).

The page and the sample bank are in [`docs/`](docs/).

## License

GPL-3.0: see [LICENSE](LICENSE).
