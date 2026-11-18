# Glag

An easy-mode generative ambient firmware for the Teenage Engineering SP-1, inspired by Charles
Vestal's [AMB-1](https://github.com/charlesvestal/fm1-amb) for the M-VAVE FM-1 and Brian Eno's
*Music for Airports*. It plays itself from the moment it is switched on: four channels, each holding
one of sixteen voices, drawing on one scale and one slowly drifting chord, through a long tape echo
and one of four reverbs. You don't perform it, you tend it.

It runs on [Wakes](https://github.com/Worldwave/Wakes)' SP-1 base (power, watchdog, standby,
battery, controls, LEDs, audio bring-up), used unchanged except where marked `CONFIG_SP1_GLAG`.

**Status: 0.5.0, not yet run on an SP-1.** It builds, passes Wakes' image safety checks, its
controls pass 87 tests on a computer, and the browser emulator runs the same engine and controls.
The first hardware session decides the rest; see [Testing on the device](#testing-on-the-device).

## The voices

| Voice | What it does | Character (channel page F1) |
|---|---|---|
| Pad | 2 or 3 voices breathing in and out on unequal cycles, each taking its note of the chord | warm (1:1) to hollow (2:1), detune |
| Bells | four loops of 13, 17, 23 and 29 beats, up to five notes each (as AMB-1's), soft FM bells, each up to 60 ms late like a human hand | ratio set: harmonic, bell, glass |
| Bass | sine and sub-octave on the chord's root, gliding | glide time and how often it takes the fifth |
| Piano | sparse 2-op FM electric piano | tine: 1:1, 2:1, 3:1 |
| Marimba | short inharmonic FM | ratio 2.5 to 5 |
| Drone | two FM pairs on the root and an interval, the index swelling over tens of seconds | interval: fifth, fourth, octave, ninth |
| Kalimba | Karplus-Strong plucks | brightness |
| Flute | one feedback-FM operator, slow attack, vibrato | vibrato |
| Arp | glassy FM walking the chord | pattern: up, up-down, random, pedal |
| Swell | brass chord swells: two detuned carriers, brightness rising with the swell, vibrato arriving late | detune and vibrato depth |
| EP | 1980s FM electric piano: a body modulator and a 14:1 tine that rings only on the attack; TONE is dull to bright, and harder notes are brighter | tine amount |
| Gong | low inharmonic gongs, struck on each new chord: a pitch dip on the strike, the index blooming for 0.6 s and fading over seconds | metal: ratio 1.4, 1.73, 2.76, 3.14 |
| Pizz | pizzicato arp: very short FM plucks up and down the chord, accents on the beat | octaves: none, alternating, random jumps |
| Shimmer | buzzy feedback FM that swoops up into each note, fast vibrato and tremolo, echoed an octave up a dotted eighth later | buzz (modulator feedback) |
| Tails | FM modulation tails: each note a random ratio, its index falling, blooming or rising over seconds; some an octave up | tail: falls, blooms, rises |
| Pulse | a light pulse, as AMB-1's: a thump on the chord's root, a closed hat and a long open hat on euclidean rings of 16, 12 and 7 sixteenths. Activity is the hits (up to 5, 7 and 3), tone the hats, length the thump's decay, register its octave | swing (the off sixteenths up to half a step late) |

Every channel page reads the same four faders: **character, tone, length, register**. The extended
page: **echo send, reverb send, pan, activity**. A fader means the same thing on every voice, except
character, which is each voice's own.

**The channel macro.** On a channel or extended page the rocker drives that channel's macro: a slow
random glide that pushes the voice's own controls around (tone, character, length, activity) and
adds tremolo and octave leaps, each weighted to suit the voice (EP gets suitcase tremolo, Pizz and Arp
leap octaves, Bells change their metal, Shimmer's buzz wanders). Four steps; at full it is twice as
wide as at half, with two-octave leaps. It sits outside the piece: saving or randomizing leaves it.

Boots on piece 1, *Garden*. Sixteen pieces. The first eight follow the stock pieces of
[AMB-1](https://github.com/charlesvestal/fm1-amb) (Charles Vestal, MPL-2.0), in Glag's voices: its
pad, bells, bass and pulse as channels 1 to 4, its settings mapped onto Glag's controls (the numbers
only; no AMB-1 code), each with a reverb chosen for it: Garden, Departures, Voices, Afternoon, Night
Bus, Shoreline, Glass and Tides. A layer AMB-1 leaves out sits at level zero, ready to bring in. Then
eight of Glag's own: Gate Seven, Long Wait, Night Flight, Lounge, Rain City (Swell, EP, Gong,
Shimmer), Deep Water (Shimmer, Tails, Gong, Pad), Lobby 84 (EP, Pizz, Bass, Swell) and Long Haul
(Pizz, Tails, Swell, Gong).

## Controls

| Where | Control | Does |
|---|---|---|
| Home (at boot) | F1 F2 F3 F4 | tempo (20-120 BPM), delay, reverb, DJ filter (left low-pass, centre off, right high-pass) |
| | T1-T4 | open that channel's page |
| | •• + T1-T4 | the next voice on that channel (all fifteen, any channel, duplicates allowed) |
| Channel page | F1 F2 F3 F4 | character, tone, length, register (5 octave steps) |
| | FFWD tap | one step more of this channel's macro (4 steps) |
| | RWD held | the macro's glide slows: twice as long every 1.5 s, from 8 s to 64 s |
| | RWD double tap | the macro off, its glide back to 8 s |
| | •• + its own T | extended page (its LED blinks); again to come back |
| | another T | that channel's page |
| Extended page | F1 F2 F3 F4 | echo send, reverb send, pan (centre detent), activity |
| | FFWD / RWD | the macro, as on the channel page |
| Harmony page | •• + PLAY once | F1 root, F2 mode (8), F3 drift (10 s to 4 min per chord), F4 motion |
| | •• + T1-T4 | the reverb: Hall, Chamber, Shimmer, Bloom (saved with the piece) |
| | •• + T3 again (on Shimmer) | Shimmer's interval: an octave, a fifth, a minor seventh |
| Anywhere | •• held + F1-F4 | the four channel levels |
| | •• twice | Home |
| | PLAY | grow / rest (resting stops new notes; the tails ring on) |
| | •• + PLAY twice | next piece (16, then back to 1) |
| Home and Harmony | FFWD tap | freeze, latched; tap again to let go |
| | FFWD held | freeze until you let go |
| | RWD held | the record slows to 10 BPM over 10 s, pitch with it; let go and it springs back |
| Anywhere | •• + FFWD | randomize the piece (voices kept, filter left open) |
| | •• + RWD | undo the last randomize or piece change (again = redo) |
| | •• + RWD held 2 s | save the piece to its slot |
| | VOL+ / VOL- | output level (Wakes' 3 dB steps) |
| | •• + VOL- tap | distortion amount: off, 1, 2, 3, 4, off |
| | •• + VOL+ tap | distortion type: Chebyshev, Overdrive, Fuzz, Fold, XOR |
| | •• + VOL- held 3 s | distortion off, and the stack back to default (Chebyshev, off, before the effects) |
| | •• + VOL+ held 3 s | distortion before the delay and reverb (default), after them, or on the reverb only, in turn |
| | •• held 3 s | power off (Wakes', unchanged; so is the 30 s backstop) |

**Faders pick up.** After a page change a fader does nothing until it reaches the value it would
change; the play row blinks that value while you look for it.

**LEDs.** The track LEDs say where you are: all four breathing on Home, one lit on a channel page,
one blinking on its extended page, a slow chase on Harmony. The play row shows each channel's
notes, a fader's value for a second while you move it, the voice or piece number when it changes
(the LED gives 1-4; steady = 1-4, slow blink = 5-8, fast blink = 9-12, breathing = 13-15), a still glow while frozen, three flashes on a
save, and the battery when •• goes down (Wakes').

**Saves** are kept until power-off for now. Keeping them on the eMMC across power cycles is the
next step (it will reuse Wakes' storage thread rather than write the nRF's flash, whose last page
the bootloader may own).

## The space

**Tape echo** (DELAY on Home): a loop of up to 4 s, as in Discreet Music. Its length is the piece's
echo time (a dotted quarter, a half, a dotted half, a bar, or two bars; a bar in most pieces),
halved until it fits. Two heads, the loop on the left and three quarters of it on the right; each
repeat comes back darker and thinner, with a little wow. Its output also feeds the reverb.

Both run below 48 kHz, so each sits between two-pole low-passes that keep what is above their reach from folding down or leaking up as a faint tinny noise.

**Four reverbs** (REVERB on Home sets how long they ring; •• + T1-T4 on Harmony picks one):

| | Reverb | Character |
|---|---|---|
| T1 | Hall | an eight-line FDN: 40 ms predelay, the brightest, barely any wobble, lows outlasting highs |
| T2 | Chamber | a four-line FDN: 8 ms predelay, mid-dark, a little wobble, the closest |
| T3 | Shimmer | a pitch shifter in the loop: the tail climbs by an octave (open, after Eventide and Strymon), a fifth or a minor seventh; •• + T3 again steps it |
| T4 | Bloom | six long allpasses in front: notes swell in late, the darkest, the most movement |

Plucked and struck voices go into the reverb bright; held voices (Pad, Bass, Drone, Flute, Swell,
Gong, Shimmer, Tails, Pulse) go in through a 12 dB/octave low-pass at 1.5 kHz, and 3 dB less per
octave above C4, so drones never turn shrill (AMB-1 darkens its pad the same way). Freeze holds any
of them as long as you like: a slow servo keeps the tail's level, and Shimmer keeps climbing.

## Distortion

Five cheap waveshapers, outside the pieces (randomize and piece changes leave them alone):

| Type | What it does |
|---|---|
| Chebyshev | a harmonic exciter: the signal is normalised by its own level, Chebyshev T2 adds the octave and T3 the twelfth, and the level is put back, so it colours without crushing |
| Overdrive | soft saturation, warmer and denser with each amount |
| Fuzz | a biased hard clip: uneven halves, a buzz |
| Fold | a triangle wavefolder: past the edge the wave turns back on itself |
| XOR | the signal crushed to fewer bits (11 down to 5) and XORed with its own last sample: digital grit |

Each amount is driven harder and brought back down, so a step changes the colour more than the
loudness (within about 1 dB of clean at amount 1, +0.75 dB per step). **Before the effects**, the
middle of the stereo image and the echo and reverb sends are distorted, so the delay and reverb
carry it; **after**, the whole mix is, ahead of the DJ filter (which can tame a fuzz). Every click
shows both positions: the type on the track row (the number picture) and the amount as a bar on
the play row, filling from the •• end before the effects and from the PLAY end after (off: one dim
LED at that end). While •• + VOL is held, the play row fills toward the 3 s mark.

Distortion costs about 6-10 % of the CPU in the model, so while it is on the voice budget gives up
two voices.

## The manual

`manual/glag-manual.html` is the visual manual: an interactive control map (choose a page and the
faders, buttons and lights change), how the pages connect, every gesture, distortion, the light
language, the voices, the pieces and power. Open it in any browser.

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

Runs the 87 control tests and renders all sixteen pieces to WAVs in `firmware/host/build/`.
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

Output: `build/zephyr/glag-sp1.bin` (about 130 KB of the 892 KB slot; 189 KB of RAM). Both Zephyr
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

If a combination runs over, `FM_CAP` in `glag/glag_engine.c` (18 half-voices, i.e. 9 voices) is the knob.

## CPU

From `host/cpu`: the engine cross-compiled with the SP-1's compiler and run under an emulated
Cortex-M4F, cycles weighted per instruction. Calibrated against Plaits, which Wakes measured on
real SP-1s: hardware cost was 1.25 to 1.65 times this model (about 1.4 typically).

| Four channels of (with the Hall, the heaviest reverb) | Model, normal settings | Model, worst block, everything at maximum | Hardware estimate (x1.4) |
|---|---|---|---|
| Pad, Bells, Bass, Flute (piece 1) | 44 % | 56 % | 61-79 % |
| Pad x 4 | 49 % | 59 % | 68-83 % |
| Bells x 4 | 43 % | 67 % | 60-93 % |
| Bass x 4 | 45 % | 48 % | 63-68 % |
| Piano x 4 | 37 % | 56 % | 51-78 % |
| Marimba x 4 | 37 % | 56 % | 51-78 % |
| Drone x 4 | 53 % | 54 % | 75 % |
| Kalimba x 4 | 36 % | 55 % | 50-77 % |
| Flute x 4 | 38 % | 46 % | 53-65 % |
| Arp x 4 | 46 % | 63 % | 64-88 % |
| Swell x 4 | 50 % | 58 % | 70-81 % |
| EP x 4 | 36 % | 57 % | 51-80 % |
| Gong x 4 | 46 % | 52 % | 64-72 % |
| Pizz x 4 | 41 % | 60 % | 58-84 % |
| Tails x 4 | 38 % | 59 % | 53-83 % |
| Pulse x 4 | 46 % | 56 % | 65-79 % |
| Arp, Swell, EP, Gong | 44 % | 57 % | 62-80 % |
| Bells x 4 + distortion at 4 | | 68 % | 95 % |

The other reverbs cost 2 to 6 points less (Chamber, the lightest, about 6). The heaviest case is four
Bells channels at full activity with distortion on, under the Hall: about 95 % on the estimate. If it
overruns on the device, lower `FM_CAP`.

Where it goes: the voices run at 24 kHz and are brought back up to 48 kHz after the mix (about half
their cost at full rate); the tape echo runs at 8 kHz and the reverbs at 12 kHz, all in 16-bit
memory. A voice budget keeps at most the cost of 9 two-operator voices sounding at once (Swell and EP,
with a third oscillator, count 1.5); the quietest decaying note gives way, and the pad, swell, drone
and bass are never cut (a pad or swell breath waits instead). Controls run at 1 kHz.

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
