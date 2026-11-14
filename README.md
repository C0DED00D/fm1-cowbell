# fm1-cowbell
Remember that time when Bad Gear asked Korg to make a Volca Cowbell way back in 2023?  
![A pink "volca cowbell" groovebox with a giant MORE knob and FEVER and PRESCRIPTION knobs](docs/cowbell_meme.png)

Korg was too scared of it's awesomeness, so now that the FM-1 supports custom firmware, I decided to take it into my own hands and create an FM-1 Firmware modeled after the meme! 

Cowbell features the giant more knob for ratchets, fever, which controls pitch, and prescription which selects one of 13 presets. The presets are recorded from a Roland TR-83 and Alesis DM-10 module to provide a variety of cowbell sounds. 

The firmware only comes with 22 KHz sample sounds, but you can use the sample uploader to get better quality samples.  

## Sample uploader

**https://c0ded00d.github.io/fm1-cowbell/**

The uploader page sends the full-quality set (44.1 kHz, 143 KB) to the FM-1 over USB-MIDI from the
browser (Chrome or Edge). The samples stay through power-offs and firmware updates.

In order to use it:
1. Plug the FM-1 in over USB with COWBELL running, and close other programs that use its MIDI port.
2. Open the page, press **Connect to the FM-1**, allow MIDI access.
3. Press **Send to the FM-1** (about ten seconds).

The page and the sample bank are in [`docs/`](docs/).

## License

GPL-3.0: see [LICENSE](LICENSE).
