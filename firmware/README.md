# Glag

An easy-mode generative ambient firmware for the Teenage Engineering SP-1, inspired by Charles
Vestal's [AMB-1](https://github.com/charlesvestal/fm1-amb) for the M-VAVE FM-1 and Brian Eno's
*Music for Airports*. It plays itself from the moment it is switched on: four channels, each holding
one of nine voices, drawing on one scale and one slowly drifting chord, through an echo and a
reverb. You don't perform it, you tend it.

It runs on [Wakes](https://github.com/Worldwave/Wakes)' SP-1 base (power, watchdog, standby,
battery, controls, LEDs, audio bring-up), used unchanged except where marked `CONFIG_SP1_GLAG`.

**Status: 0.1.0, not yet run on an SP-1.** It builds, passes Wakes' image safety checks, its
controls pass 49 tests on a computer, and the browser emulator runs the same engine and controls.
The first hardware session decides the rest; see [Testing on the device](#testing-on-the-device).

## The voices

| Voice | What it does | Character (extended F4) |
|---|---|---|
| Pad | 2 or 3 voices breathing in and out on unequal cycles, each taking its note of the chord | warm (1:1) to hollow (2:1), detune |
| Bells | four tape loops of 13, 17, 23 and 29 beats, soft FM bells that drift against each other | ratio set: harmonic, bell, glass |
| Bass | sine and sub-octave on the chord's root, gliding | glide time and how often it takes the fifth |
| Piano | sparse 2-op FM electric piano | tine: 1:1, 2:1, 3:1 |
| Marimba | short inharmonic FM | ratio 2.5 to 5 |
| Drone | two FM pairs on the root and an interval, the index swelling over tens of seconds | interval: fifth, fourth, octave, ninth |
| Kalimba | Karplus-Strong plucks | brightness |
| Flute | one feedback-FM operator, slow attack, vibrato | vibrato |
| Arp | glassy FM walking the chord | pattern: up, up-down, random, pedal |

Every channel page reads the same four faders: **activity, tone, length, register**. The extended
page: **echo send, reverb send, pan, character**. A fader means the same thing on every voice.

Boots on piece 1, *Terminal*: Pad, Bells, Bass, Flute. Eight pieces: Terminal, Gate Seven, Long
Wait, Jet Lag, Tarmac, Night Flight, Arrivals, Lounge.

## Controls

| Where | Control | Does |
|---|---|---|
| Home (at boot) | F1 F2 F3 F4 | tempo (30-120 BPM), delay, reverb, DJ filter (left low-pass, centre off, right high-pass) |
| | T1-T4 | open that channel's page |
| | •• + T1-T4 | the next voice on that channel (all nine, any channel, duplicates allowed) |
| Channel page | F1 F2 F3 F4 | activity, tone, length, register (5 octave steps) |
| | •• + its own T | extended page (its LED blinks); again to come back |
| | another T | that channel's page |
| Extended page | F1 F2 F3 F4 | echo send, reverb send, pan (centre detent), character |
| Harmony page | •• + PLAY once | F1 root, F2 mode (8), F3 drift (10 s to 4 min per chord), F4 motion |
| Anywhere | •• held + F1-F4 | the four channel levels |
| | •• twice | Home |
| | PLAY | grow / rest (resting stops new notes; the tails ring on) |
| | •• + PLAY twice | next piece |
| | FFWD tap | freeze, latched; tap again to let go |
| | FFWD held | freeze until you let go |
| | RWD held | the record slows to 10 BPM over 10 s, pitch with it; let go and it springs back |
| | •• + FFWD | randomize the piece (voices kept, filter left open) |
| | •• + RWD | undo the last randomize or piece change (again = redo) |
| | •• + RWD held 2 s | save the piece to its slot |
| | VOL+ / VOL- | output level (Wakes' 3 dB steps) |
| | •• held 3 s | power off (Wakes', unchanged; so is the 30 s backstop) |

**Faders pick up.** After a page change a fader does nothing until it reaches the value it would
change; the play row blinks that value while you look for it.

**LEDs.** The track LEDs say where you are: all four breathing on Home, one lit on a channel page,
one blinking on its extended page, a slow chase on Harmony. The play row shows each channel's
notes, a fader's value for a second while you move it, the voice or piece number when it changes
(LED 1-4 lit = 1-4, blinking = 5-8, all four = 9), a still glow while frozen, three flashes on a
save, and the battery when •• goes down (Wakes').

**Saves** are kept until power-off in 0.1.0. Keeping them on the eMMC across power cycles is the
next step (it will reuse Wakes' storage thread rather than write the nRF's flash, whose last page
the bootloader may own).

## The emulator

`web/` builds the engine and the controls to WebAssembly and runs them in a page with the SP-1's
controls and the screen it doesn't have: what each fader does now and where to pick it up, each
channel's voice, and an estimated worst-case CPU load for the chosen voices.

```bash
sh firmware/web/build.sh
```

Open `firmware/web/build/glag-emulator.html`. Keyboard: 1-4 = T1-T4, Shift = ••, Space = PLAY,
`[` = RWD, `]` = FFWD, `-` `=` = VOL.

## On a computer

```bash
sh firmware/host/run_tests.sh
```

Runs the 49 control tests and renders all eight pieces to WAVs in `firmware/host/build/`.
`firmware/host/cpu/` runs the engine, cross-compiled for the Cortex-M4F, under an emulator and
counts cycles (see [CPU](#cpu)).

## Building the firmware

Zephyr v4.3.1 and Zephyr SDK 0.17.4, pinned (Wakes' combination). The workspace path must not
contain spaces.

```bash
mkdir sp1-ws && cd sp1-ws && git clone https://github.com/burn-again/SP-1-Glag
```

```bash
west init -l --mf firmware/west.yml SP-1-Glag && west update
```

```bash
cd zephyr && git apply ../SP-1-Glag/firmware/zephyr-patches/*.patch && cd ..
```

```bash
pip install -r zephyr/scripts/requirements.txt
```

```bash
west build -p always -b stem_player SP-1-Glag/firmware/app -- -DBOARD_ROOT=$PWD/SP-1-Glag/firmware
```

```bash
python3 SP-1-Glag/firmware/tools/ci/check_image.py build
```

Output: `build/zephyr/glag-sp1.bin` (about 109 KB of the 892 KB slot; 139 KB of RAM). Both Zephyr
patches are required and are lost on every `west update`. `check_image.py` refuses an image that
could write below 0x20000 or into the reserved page, or that drops the watchdog.

## Flashing

At your own risk: the SP-1 has no manufacturer support.

1. Open <https://solderless.engineering> in Chrome or Edge.
2. Hold **T1 + T4** and keep them held while you plug in USB-C.
3. Select `glag-sp1.bin` in the firmware utility and flash it.

T1 + T4 lives in Teenage Engineering's bootloader, below 0x20000, which Glag never writes, so it
always works: a bad build can be replaced by flashing another (Wakes, or Glag again).

## Testing on the device

Open the USB console (any serial terminal) while it runs. Every 5 s Wakes' `AUD` line reports the
audio thread's load: `cyc avg=` and `max=` are percent of the 2 ms block. Glag logs `GLAG page`,
`GLAG piece` and `GLAG channel` lines as you use it. For the first session:

1. Boot, listen, and read `AUD ... cyc avg / max` with the default voices.
2. Set all four channels to the same voice (•• + T on Home), turn activity and length up, and read
   `max` again for each voice. `fail=0` and `max` under ~90 % is the target.
3. Try every gesture in the table above, and the power-off hold while the music plays.

If a combination runs over, `FM_CAP` in `glag/glag_engine.c` (now 10 oscillator voices) is the knob.

## CPU

From `host/cpu`: the engine cross-compiled with the SP-1's compiler and run under an emulated
Cortex-M4F, cycles weighted per instruction. Calibrated against Plaits, which Wakes measured on
real SP-1s: hardware cost was 1.25 to 1.65 times this model (about 1.4 typically).

| Four channels of | Model, normal settings | Model, worst block, everything at maximum | Hardware estimate (x1.4) |
|---|---|---|---|
| Pad, Bells, Bass, Flute (piece 1) | 31 % | 49 % | 44-69 % |
| Pad x 4 | 39 % | 57 % | 54-80 % |
| Bells x 4 | 40 % | 65 % | 56-90 % |
| Drone x 4 | 49 % | 49 % | 69 % |
| Arp x 4 | 36 % | 63 % | 50-88 % |
| Kalimba x 4 | 20 % | 59 % | 28-82 % |
| Flute x 4 | 23 % | 36 % | 32-50 % |

A voice budget keeps at most 10 FM oscillators sounding at once (the quietest decaying note gives
way); the pad, drone and bass are never cut. Echo and reverb run at 24 kHz, controls at 1 kHz.

## Layout

| | |
|---|---|
| `glag/` | the engine (`glag_engine`) and the controls (`glag_ui`): plain C, no hardware |
| `app/` | the Zephyr application: Wakes' SP-1 base plus the Glag hooks in `main.c` and `sp1_audio.c` |
| `boards/`, `zephyr-patches/`, `west.yml` | Wakes' board definition, Zephyr patches and pinned manifest |
| `host/` | control tests, the WAV renderer, the CPU model |
| `web/` | the browser emulator |
| `tools/ci/check_image.py` | Wakes' image safety check |

## Credits and licences

Glag's code is MIT. The SP-1 base is Wakes by Adara Barami | Worldwave (MIT), which builds on
chattock/sp1-tape-looper, timknapen/SP-1-dev and ericlewis/sp1-midi (all MIT) and Zephyr
(Apache-2.0). The idea, and the habit of tending rather than playing, come from Charles Vestal's
AMB-1; no AMB-1 code is used. Full texts and what each covers: [`LICENSES/`](LICENSES/README.md).

Glag is not affiliated with Teenage Engineering, Worldwave, M-VAVE, Charles Vestal or Brian Eno.
