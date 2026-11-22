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
 *   BELLS     four loops of 13, 17, 23 and 29 beats, up to five notes each, soft FM bells
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
	GV_STRINGS, GV_GLOW, GV_SEQ, GV_SUPER,
	GV_RIBBON, GV_FUNK, GV_HARP, GV_HARPSI, GV_KIT,
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
#define GL_NECHO 9                      /* a dotted quarter, a half, a dotted half, a bar, two bars, an eighth, a dotted eighth, a quarter, the string */
#define GL_ECHO_STRING 8                /* the echo as a plucked string: one cycle of the chord's root */
enum { GL_SP_REVERB, GL_SP_SHIMMER, GL_SP_ECHO, GL_SP_FLAGS, GL_SP_KIT, GL_NSP = 5 };   /* GL_SP_KIT: the Kit's tuning (0 the firmware's) */
#define GL_FLAG_REVERSE 1u              /* space[GL_SP_FLAGS]: reverse swells, ping-pong echo */
#define GL_FLAG_PINGPONG 2u
/* bits 2-4: key travel (0 off, 1 up a fifth, 2 lifts, 3 relative, 4 drops); bits 5-6: the macros'
 * shape (0 glide, 1 steps on each bar, 2 a long rise) */
#define GL_FLAG_KEY(f) (((f) >> 2) & 7u)
#define GL_FLAG_SHAPE(f) (((f) >> 5) & 3u)
#define GL_NKEYWAY 5
#define GL_NSHAPE 3
/* per channel (gl_patch.chx): bits 0-1 the time division (1, 1/2, 1/3, 1/4 of the steps),
 * bits 2-4 the interval above what it would play (none, a third, fifth, seventh, ninth) */
#define GL_CHX_DIV(x) ((x) & 3u)
#define GL_CHX_IVL(x) (((x) >> 2) & 7u)
#define GL_NIVL 5
/* on a Kit channel the same bits, and bit 5, are its groove (0 euclidean, then GL_NGROOVE - 1 more) */
#define GL_CHX_GRV(x) (((x) >> 2) & 15u)
#define GL_NGROOVE 11
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
	uint8_t chx[GL_NCH];            /* per channel: time division and interval (GL_CHX_*) */
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
 * 2: on the reverb only (dirty tails, clean instruments); 3: on the drums only (the Pulse, and so
 * its echo and reverb). */
enum gl_dist { GD_CHEBYSHEV, GD_OVERDRIVE, GD_FUZZ, GD_XOR, GD_COUNT };
extern const char *const GL_DIST_NAME[GD_COUNT];
void gl_dist(int type, int amount, int post);
void gl_panic(void);
/* The channel macro: a slow random glide that pushes channel ch's own controls around (tone,
 * character, length, activity) plus tremolo and octave leaps, each weighted to suit its voice.
 * depth 0 = off .. 1 = full (twice as wide as half); period_s: seconds per glide. Outside the
 * patch: the posted controls are the centre it moves around. */
void gl_macro(int ch, float depth, float period_s);
/* Start the posted patch afresh, as at power-on with this seed: the chord walk from its home
 * chord, the dice, the clock and the slow LFOs from the start, every channel reset, the echo and
 * reverb emptied. For the album (between tracks, in silence): each piece plays as recorded. */
void gl_fresh(uint32_t seed);
/* The crank: while resting or frozen, play on to the next step where something sounds (a bar
 * at most), now. */
void gl_crank(void);
/* MIDI in: a note on channel ch (0..3) of Glag, velocity 1..127 (0: note off). Any thread: it
 * is queued and played at the next millisecond. While a channel has notes held, and for 4 s
 * after its last, MIDI plays it and its own sequencer rests. On the Pulse's channel notes are
 * drums by the note's name in any octave: C kick, D/E snare, F#/G# hat, A# open hat, C#/D# click
 * (General MIDI's 36, 38, 42, 46 land right). */
void gl_midi_note(int ch, int note, int vel);
void gl_cho_share(int m);            /* audition only: one chorus for the Strings and the Kit (1 Juno, 2 the Kit's) */
void gl_rv_drift(int mode);          /* audition only: the Hall's lines drifting (1 grow/shrink, 2 diverge, 3 grow, 4 shrink) */
void gl_rv_shim_ratio(float r);      /* audition only: the Shimmer's interval as a ratio (below 1: down; 0 its own) */
void gl_rv_fx(int mode);              /* audition only: odd tails (1 crush growing, 2 crush per pass, 3/4 endless rise/fall, 5 ring arp, 6 ring contrary, 7/8 Fibonacci ring up/down) */
void gl_rv_cut(int bits);            /* audition only: 1 the Hall on four lines, 2 one damping band */
void gl_drop_cap(int n);             /* audition only: at most n notes and hits start in a step (0 off) */
void gl_str_thin(int mask);          /* audition only: Strings thinned (bits: 1 no chorus, 2 one filter, 4 two poles, 8 one saw, 16 two notes) */
void gl_song_drums(int style);       /* song mode's drums leaving: 0 fade, 1 longer, 2 peel, 3 peel + a fill into the reverb */
void gl_kit_solo(int lanes);          /* audition only: the kit's lanes to play (bits; 0 all) */
void gl_kit_color(int method);        /* audition only: the Kit's colour (0 none, 1 comb, 2 chord resonators, 3 through-zero FM, 4 soggy, 5 metal, 6 soggy + Chebyshev: the default, 7 a comb's soggy + Chebyshev on the low end) */
void gl_kit_rotate(int always);       /* audition only: the Kit rotates on every phrase (else at full macro) */
void gl_kit_tune(int id, float v);    /* the Kit tuning page: one of its numbers (glag_kit.h, KT_*) */
int gl_kit_tunes(void);               /* how many there are */
/* the tuning page's step editor: rows (kick, snare, ghost, squash, hat, open hat, clank, tissue,
 * Nord, roll, tom root, third, fifth, high), steps 0-31, v 0 off, 1 on, 2 accent */
void gl_kit_pattern(int row, int step, int v);
void gl_kit_pattern_mode(int on, int len, float swing);
/* the Kit's tunings (sets of its numbers, glag_kit.h): a patch's space[GL_SP_KIT] picks one */
int gl_kit_ntunings(void);
const char *gl_kit_tuning_name(int t);
void gl_kit_tuning_lock(int on);      /* the tuning page: patches leave the Kit's numbers alone */
int gl_kit_pattern_pos(void);
void gl_kit_director(int on);         /* the tuning page: the Kit map wanders even while the page holds the numbers */
void gl_alt(int voice, int method);   /* audition only: another way to make a voice (0: as it is) */

/* Playworld (a prototype, outside the patch): see glag_play.h */
enum gl_world_what { GW_FORM, GW_FORM_BARS, GW_KEY_MIN, GW_KEY_KIND, GW_PUMP, GW_SHAPE, GW_PINGPONG,
                     GW_TEXTURE, GW_TEX_LEVEL, GW_REVERSE, GW_AUTOF, GW_FORM_KIND, GW_COMP,
                     /* song mode (Playworld's T1): on/off (off ends it gracefully), fills, how full,
                      * and its randomness: the settings, the macros' pace, key travel, straying */
                     GW_SONG, GW_FILLS, GW_FULL, GW_RND_SET, GW_RND_MAC, GW_RND_KEY, GW_RND_STRAY,
                     /* the pump (T3): release, bend, the drums' level (0: pumping without them), its
                      * rhythm (0 the kick, 1 beats, 2 eighths, 3 half bars), how much the space
                      * ducks, how much bass and held voices are spared */
                     GW_PUMP_REL, GW_PUMP_BEND, GW_PUMP_DRUMS, GW_PUMP_RHYTHM, GW_PUMP_SPACE, GW_PUMP_SPARE,
                     GW_KEY_HOLD /* 1: the key stays the piece's own, whatever key travel or song mode say (the album) */,
                     /* arp mode (T2): on/off (taken on the next bar), octaves, pattern (up, down,
                      * up-down, random), division (16ths, 8ths, dotted 8ths, quarters), spread */
                     GW_ARP, GW_ARP_OCT, GW_ARP_PAT, GW_ARP_DIV, GW_ARP_SPREAD };
/* per channel (or per texture control): song mode's stagger and loop length (0..4: x1/4 .. x4),
 * the chosen texture's three controls past its level */
enum gl_world_ch_what { GWC_STAGGER, GWC_LOOP, GWC_TEXP };
void gl_world_ch(int what, int i, float v);
void gl_world(int what, float v);
/* echo times in order from shortest to longest (for stepping) */
extern const uint8_t GL_ECHO_ORDER[GL_NECHO];

/* ---- audio thread ---- */
/* n stereo frames, interleaved int16, n a multiple of GL_SUB. gain 0..1 (volume). */
void gl_render(int16_t *lr, uint32_t n, float gain);
/* After each block: its share (0..1) of the time the block had. Over 88 % the engine sounds
 * fewer notes at once until it cools (the voice budget comes down, a voice at a time). */
void gl_load(float frac);

/* ---- state for the LEDs (written by the render, read anywhere) ---- */
extern volatile float gl_glow[GL_NCH];   /* 0..1: this channel just played a note */
extern volatile float gl_beat;           /* 0..1 phase of the current beat */
extern volatile uint8_t gl_chord_root;   /* pitch class of the current chord */
extern volatile uint8_t gl_playing, gl_frozen;
extern volatile float gl_speed;          /* 1 = normal, less while slowing */
extern volatile float gl_peak;           /* output peak, 0..1, decays */

/* ---- pieces ---- */
#ifdef GL_DEMO_PIECES
#define GL_NPIECES 118                  /* an audition build: a hundred and two more (five of them labs) */
#else
#define GL_NPIECES 16
#endif
const char *gl_piece_name(int i);
void gl_piece(int i, gl_patch *out);
/* the piece's channel macros: steps 0..4 per channel, and seconds per glide */
void gl_piece_macro(int i, uint8_t *steps, float *period_s);
/* its hidden macro scene, the same way (brought in by hand) */
void gl_piece_scene(int i, uint8_t *steps, float *period_s);
/* A tasteful random patch, keeping the current voices and harmony root unless asked. */
void gl_randomize(gl_patch *p, uint32_t seed);

/* ---- pure helpers (tests) ---- */
int gl_scale(int mode, int *pcs);        /* the mode's pitch classes, ascending; count */

#ifdef __cplusplus
}
#endif
#endif
