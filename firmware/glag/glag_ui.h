/* SPDX-License-Identifier: MIT
 * Glag controls: pages, soft takeover, gestures and LEDs. Hardware-independent: the firmware,
 * the PC emulator and the browser emulator all feed it the same inputs.
 *
 * ---- the map ----
 *   HOME (at boot)       F1 tempo   F2 delay   F3 reverb   F4 DJ filter (centre = off)
 *                        •• held + F1..F4: the four channel levels (on every page)
 *                        T1..T4: open that channel's page
 *                        •• + T1..T4: the next voice on that channel (16 voices, any channel)
 *   CHANNEL page         F1 character  F2 tone  F3 length  F4 register (5 octave steps)
 *                        •• + its own T: extended page (blinking LED), again to come back
 *   EXTENDED page        F1 echo send  F2 reverb send  F3 pan (centre detent)  F4 activity
 *                        another T jumps straight to that channel's page
 *   CHANNEL + EXTENDED   the rocker is that channel's macro (a slow random glide over its own
 *                        controls, tremolo and octave leaps):
 *                        FFWD tap: one step more (4 steps, shown as a bar)
 *                        RWD held: the glide slows, doubling every 1.5 s, down to one per 64 s
 *                        RWD double tap: the macro off, its speed back to the start
 *   HARMONY page         •• + PLAY once: F1 root (12 steps)  F2 mode (8)  F3 drift  F4 motion
 *                        •• + T1..T4: the reverb: Hall, Chamber, Shimmer, Bloom (saved with the
 *                        piece); •• + T3 again on Shimmer: its interval, octave / fifth / seventh
 *                        (the track row shows the reverb, the play row Shimmer's interval)
 *   PLAYWORLD            PLAY held 2 s (from anywhere; again to leave, or •• double tap):
 *                        all lights off until something is on. A bright light: that section is
 *                        on; a dim one, pulsing: the page its faders are on (the last T pressed
 *                        is the one the faders follow; others stay on). Each press shows itself
 *                        for a second (a light gliding to the page, or fading out).
 *                        T1 song mode: on (page 1), page 2, 3, 4, off (an outro, then the piece)
 *                           1: F1-F4 each channel's stagger    2: each channel's loop (x1/4..x4)
 *                           3: arrangement, length, fills, how full   4: randomness: settings,
 *                              the macros' pace, key travel, straying
 *                        T2 arp mode: on, off (each taken on the next bar): every channel but the
 *                           drums played by one arp in its own voice, at its shortest decay and
 *                           sustain. F1 octaves (1-4), F2 pattern (up, down, up-down, random),
 *                           F3 division (16ths, 8ths, dotted 8ths, quarters), F4 spread (0: all on
 *                           the same note at once; up: each further along; past 2/3 half speed)
 *                        T3 pump: on (page 1), page 2, off
 *                           1: depth, release, bend, drums (down: the pump goes on without them)
 *                           2: master compressor, rhythm (kick, beats, eighths, half bars),
 *                              how much the space ducks, how much bass and held voices are spared
 *                        T4 texture: rain, vinyl, tape hiss, surf, off; F1 level, F2-F4 its own
 *                        Song changes land on the next bar and glide; leaving keeps all of it on.
 *   SHUFFLE              PLAY held 20 s, then let go: every piece in a random order (no repeats
 *                        until all have played), two minutes each, each with random settings: song
 *                        mode with its own arrangement, and by chance distortion, the pump (now and
 *                        then without drums), a texture, another reverb, reverse, ping-pong, an echo
 *                        time, the auto filter, macros. Controls as the album's.
 *   DEMO                 PLAY held 10 s, then let go (from anywhere): the album. The AMB-1
 *                        pieces in order, two minutes each, fading from one to the next, round
 *                        and round, each in its own key (song mode only if it was already on, and
 *                        its key travel held off). The track row shows
 *                        the piece's number. FFWD: the next piece; RWD: the one before; any
 *                        other press (T1-T4, PLAY) ends it, staying on the piece playing.
 *   anywhere             •• double tap: HOME
 *                        PLAY: grow / rest
 *                        •• + PLAY twice: next piece
 *   HOME                 FFWD tap: freeze on/off (latched); FFWD held: freeze until released
 *                        RWD held: the record slows to 10 BPM over 10 s; release springs back
 *   HARMONY              the rocker alone does nothing (kept for later)
 *   HOME (only; elsewhere •• + the rocker is kept for later)
 *                        •• + FFWD: randomize the piece        •• + RWD: undo (randomize or piece)
 *                        •• + FFWD held 3 s: the piece's macro scene on all four channels
 *                        •• + RWD held 3 s: save the piece to its slot, its macros as its scene
 *   anywhere
 *                        VOL+ / VOL-: volume
 *                        •• + VOL- tap: distortion amount, off then 1-4
 *                        •• + VOL+ tap: distortion type (Chebyshev, overdrive, fuzz, fold, XOR)
 *                        •• + VOL- held 3 s: distortion off, the stack back to default
 *                        •• + VOL+ held 3 s: distortion before the delay and reverb, after them,
 *                        or on the reverb only (in turn)
 *                        (each click shows the type on the track row and the amount on the
 *                        play row, filling from the •• end before the effects, PLAY end after,
 *                        blinking on the reverb only)
 *
 * Not here, on purpose: the power gestures (•• held 3 s, the 30 s backstop) and the battery
 * picture belong to the firmware's power code and are never routed through this file.
 *
 * Soft takeover: after a page change a fader does nothing until it reaches the value it would
 * change (the play row blinks that value while you look for it), then it takes over.
 */
#ifndef GLAG_UI_H
#define GLAG_UI_H

#include <stdint.h>
#include "glag_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

enum gl_btn { GB_T1, GB_T2, GB_T3, GB_T4, GB_PLAY, GB_VOLUP, GB_VOLDN, GB_FFWD, GB_RWD, GB_FN, GL_NBTN };

typedef struct {
	float fader[4];          /* 0..1, F1..F4, already smoothed */
	uint16_t buttons;        /* bit (1 << enum gl_btn): held now, debounced */
} gl_input;

enum gl_page { PG_HOME, PG_CH, PG_EXT, PG_HARMONY, PG_WORLD };

typedef struct {
	uint8_t led_t[4];        /* track LEDs T1..T4, 0..255 */
	uint8_t led_p[4];        /* play row, from the •• end to the PLAY end, 0..255 */
	float volume;            /* 0..1: pass to gl_render() */
	uint8_t save_slot;       /* 1..16: persist glui_saved(save_slot - 1) now; 0 = nothing */
	uint8_t page, channel;   /* where the UI is (enum gl_page, 0..3) */
	uint8_t piece;           /* 0..15 */
	uint8_t dist_type, dist_amt, dist_post;   /* enum gl_dist, 0 off .. 4; 0 before the effects, 1 after, 2 the reverb only */
	uint8_t reverb, shimmer;  /* enum gl_reverb, enum gl_shimmer */
	uint8_t macro[4];        /* each channel's macro depth, 0 off .. 4 */
	float macro_period[4];   /* and its glide, in seconds */
	/* Playworld: song mode (0 off, 1-4 its page), arp (0/1), pump (0 off, 1-2), texture
	 * (0 off, 1 rain .. 4 surf), and which of the four the faders follow (-1: none) */
	uint8_t world_song, world_arp, world_pump, world_tex;
	int8_t world_focus;
	/* demo mode: 1 the album (PLAY held 10 s), 2 shuffle (20 s); how far into this piece (ms) */
	uint8_t demo;
	uint32_t demo_ms;
} gl_ui_out;

void glui_init(void);
/* The device has just turned ON again (it was off or in standby): take the faders as they now
 * are, rather than as a movement from where they were. Values and the page are kept. */
void glui_enter(void);
/* Give the UI a piece's saved version (at boot, from storage). NULL clears it. */
void glui_set_saved(int piece, const gl_patch *p);
const gl_patch *glui_saved(int piece);  /* NULL if that piece was never saved */
const gl_patch *glui_patch(void);       /* what is playing */
/* What fader f (0..3) controls right now: its stored value, and whether the fader has picked
 * it up yet (soft takeover). For emulators and logs. */
float glui_target(int f, int *caught);
/* Call every few milliseconds. Posts to the engine itself. */
void glui_tick(uint32_t now_ms, const gl_input *in, gl_ui_out *out);

#ifdef __cplusplus
}
#endif
#endif
