/* SPDX-License-Identifier: MIT
 * Glag: an easy-mode generative ambient engine for the SP-1.
 *
 * Inspired by AMB-1 (Charles Vestal, for the M-VAVE FM-1) and Brian Eno's Music for Airports,
 * written from scratch for the nRF52840 (64 MHz Cortex-M4F, 256 KB RAM). No code from AMB-1.
 *
 * Four channels. Each channel holds one of sixteen voices, and any voice can sit on any channel
 * (duplicates allowed). They all draw on one scale and one chord that drifts slowly through it.
 *
 *   PAD       breathing chord: voices swell in and out on cycles of unequal length
 *   BELLS     four tape loops of 13, 17, 23 and 29 beats, each a soft FM bell
 *   BASS      sine + sub-octave on the chord root, gliding
 *   PIANO     sparse 2-op FM electric piano
 *   MARIMBA   inharmonic 2-op FM, short and glassy
 *   DRONE     two FM pairs on root and fifth, index swelling over ~40 s
 *   KALIMBA   Karplus-Strong plucks
 *   FLUTE     one feedback-FM operator, slow attack, vibrato
 *   ARP       glassy FM arpeggio over the chord
 *   SWELL     brass chord swells: two detuned carriers, brightness following the swell, late vibrato
 *   EP        1980s FM electric piano: body plus a 14:1 tine on the attack, dull to bright
 *   GONG      low inharmonic gongs: a pitch dip on the strike, the index blooming then fading
 *   PIZZ      pizzicato arp: short FM plucks over the chord, with octave jumps
 *   SHIMMER   buzzy feedback FM that swoops up into each note, fast vibrato and tremolo, an octave echo
 *   TAILS     FM modulation tails: each note's index falls, blooms or rises over seconds
 *   PULSE     a light pulse: soft kick, shaker and wood click on euclidean rings of 16, 12 and 7
 *
 * Every voice reads the same eight controls, so a fader means the same thing on every page:
 *   page:      ACTIVITY  TONE  LENGTH  REGISTER
 *   extended:  ECHO      VERB  PAN     CHARACTER   (character is the one voice-specific knob)
 * plus each channel's LEVEL (Home, shift layer).
 *
 * Global: TEMPO, DELAY, REVERB, FILTER (DJ: left low-pass, centre off, right high-pass),
 * and the harmony: ROOT, MODE, DRIFT (how often the chord moves), MOTION (the slow arc and
 * how much the music evolves).
 *
 * Threads: gl_render() runs in the audio thread. Everything else is called from the main
 * thread and only posts: the render picks posted state up at the start of its next call.
 * Float only; no libm calls in the audio path.
 */
#ifndef GLAG_ENGINE_H
#define GLAG_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GL_SR 48000.0f
#define GL_SUB 48u                    /* control-rate sub-block, 1 ms */
#define GL_NCH 4

enum gl_voice {
	GV_PAD, GV_BELLS, GV_BASS, GV_PIANO, GV_MARIMBA, GV_DRONE, GV_KALIMBA, GV_FLUTE, GV_ARP,
	GV_SWELL, GV_EP, GV_GONG, GV_PIZZ, GV_SHIMMER, GV_TAILS, GV_PULSE,
	GV_COUNT
};
extern const char *const GL_VOICE_NAME[GV_COUNT];

/* per-channel controls, all 0..1 */
enum gl_cp {
	CP_ACTIVITY, CP_TONE, CP_LENGTH, CP_REGISTER,   /* page     */
	CP_ECHO, CP_VERB, CP_PAN, CP_CHAR,              /* extended */
	CP_LEVEL,                                       /* mixer    */
	GL_NCP
};
/* global controls, all 0..1 */
enum gl_gp {
	GP_TEMPO, GP_DELAY, GP_REVERB, GP_FILTER,       /* Home     */
	GP_ROOT, GP_MODE, GP_DRIFT, GP_MOTION,          /* Harmony  */
	GL_NGP
};

#define GL_NMODES 8
extern const char *const GL_MODE_NAME[GL_NMODES];

/* The space: which reverb, the shimmer's interval, and the tape echo's length. */
enum gl_reverb { GR_HALL, GR_CHAMBER, GR_SHIMMER, GR_BLOOM, GR_COUNT };
enum gl_shimmer { GS_OCTAVE, GS_FIFTH, GS_SEVENTH, GS_COUNT };
#define GL_NECHO 5                      /* a dotted quarter, a half, a dotted half, a bar, two bars */
enum { GL_SP_REVERB, GL_SP_SHIMMER, GL_SP_ECHO, GL_NSP = 4 };
extern const char *const GL_REVERB_NAME[GR_COUNT];
extern const char *const GL_SHIMMER_NAME[GS_COUNT];
extern const char *const GL_ECHO_NAME[GL_NECHO];

/* Everything a piece is. Plain data: the UI owns it, saves it, posts copies of it. */
typedef struct {
	uint8_t voice[GL_NCH];          /* enum gl_voice */
	float ch[GL_NCH][GL_NCP];
	float g[GL_NGP];
	uint8_t space[GL_NSP];          /* [GL_SP_REVERB] gl_reverb, [GL_SP_SHIMMER] gl_shimmer,
	                                 * [GL_SP_ECHO] 0..GL_NECHO-1 */
} gl_patch;

/* Stepped controls: how many positions they have (the UI snaps faders to these). 0 = smooth. */
int gl_cp_steps(int cp);
int gl_gp_steps(int gp);
/* Controls with a centre detent (pan, filter). */
int gl_cp_centred(int cp);
int gl_gp_centred(int gp);
float gl_tempo_bpm(float v);        /* GP_TEMPO 0..1 -> 20..120 BPM */

/* ---- main thread ---- */
void gl_init(uint32_t seed);
void gl_post_patch(const gl_patch *p);   /* the whole patch; picked up at the next render */
void gl_play(int on);                    /* grow / rest: rest stops new notes, tails ring on */
void gl_freeze(int on);                  /* hold the chord, stop new notes, reverb rings forever */
void gl_slow(int on);                    /* record slowing to 10 BPM over 10 s; off springs back */
void gl_next_chord(void);
/* Distortion, outside the patch (pieces and randomize leave it alone). amount 0 = off, 1..4.
 * post 0: on the instruments, before the delay and reverb; 1: on everything, after them;
 * 2: on the reverb only (dirty tails, clean instruments). */
enum gl_dist { GD_CHEBYSHEV, GD_OVERDRIVE, GD_FUZZ, GD_FOLD, GD_XOR, GD_COUNT };
extern const char *const GL_DIST_NAME[GD_COUNT];
void gl_dist(int type, int amount, int post);
void gl_panic(void);
/* The channel macro: a slow random glide that pushes channel ch's own controls around (tone,
 * character, length, activity) plus tremolo and octave leaps, each weighted to suit its voice.
 * depth 0 = off .. 1 = full (twice as wide as half); period_s: seconds per glide. Outside the
 * patch: the posted controls are the centre it moves around. */
void gl_macro(int ch, float depth, float period_s);

/* ---- audio thread ---- */
/* n stereo frames, interleaved int16, n a multiple of GL_SUB. gain 0..1 (volume). */
void gl_render(int16_t *lr, uint32_t n, float gain);

/* ---- state for the LEDs (written by the render, read anywhere) ---- */
extern volatile float gl_glow[GL_NCH];   /* 0..1: this channel just played a note */
extern volatile float gl_beat;           /* 0..1 phase of the current beat */
extern volatile uint8_t gl_chord_root;   /* pitch class of the current chord */
extern volatile uint8_t gl_playing, gl_frozen;
extern volatile float gl_speed;          /* 1 = normal, less while slowing */
extern volatile float gl_peak;           /* output peak, 0..1, decays */

/* ---- pieces ---- */
#define GL_NPIECES 12
const char *gl_piece_name(int i);
void gl_piece(int i, gl_patch *out);
/* A tasteful random patch, keeping the current voices and harmony root unless asked. */
void gl_randomize(gl_patch *p, uint32_t seed);

/* ---- pure helpers (tests) ---- */
int gl_scale(int mode, int *pcs);        /* the mode's pitch classes, ascending; count */

#ifdef __cplusplus
}
#endif
#endif
