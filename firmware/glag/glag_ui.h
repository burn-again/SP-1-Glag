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
 *   anywhere             •• double tap: HOME
 *                        PLAY: grow / rest
 *                        •• + PLAY twice: next piece
 *   HOME + HARMONY       FFWD tap: freeze on/off (latched); FFWD held: freeze until released
 *                        RWD held: the record slows to 10 BPM over 10 s; release springs back
 *   anywhere
 *                        •• + FFWD: randomize the piece        •• + RWD: undo (randomize or piece)
 *                        •• + RWD held 2 s: save the piece to its slot
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

enum gl_page { PG_HOME, PG_CH, PG_EXT, PG_HARMONY };

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
