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
	/* audition (glag_new.h): nine light voices, after the rest so no piece changes */
	GV_LITEKIT, GV_PERC, GV_ORGAN, GV_MBOX, GV_BREATH, GV_CHIP, GV_CHOIR, GV_KOTO, GV_GRAINS,
	/* audition (glag_new2.h): six more */
	GV_TRUMPET, GV_TIMPANI, GV_SAX, GV_CELLO, GV_UPRIGHT, GV_HAZE, GV_VIBES, GV_BRUSH, GV_RHODES,
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
void gl_fresh_keep(uint32_t seed);   /* the same, the echo and reverb left ringing (the noir album) */
/* The crank: while resting or frozen, play on to the next step where something sounds (a bar
 * at most), now. */
void gl_crank(void);
/* MIDI in: a note on channel ch (0..3) of Glag, velocity 1..127 (0: note off). Any thread: it
 * is queued and played at the next millisecond. While a channel has notes held, and for 4 s
 * after its last, MIDI plays it and its own sequencer rests. On the Pulse's channel notes are
 * drums by the note's name in any octave: C kick, D/E snare, F#/G# hat, A# open hat, C#/D# click
 * (General MIDI's 36, 38, 42, 46 land right). */
void gl_midi_note(int ch, int note, int vel);
void gl_midi_keep(int mask);
/* audition: a space piece's tide (its sections of quiet and busy, sends and space moving).
 * gl_tide(0, on), (1, channel: a throw into the echo now), (2, 0: on to the next section);
 * gl_tide_info: the section, how many, its kind (GL_TIDE_KIND), the bar in it and its length */
void gl_tide(int what, float v);
int gl_tide_info(int *sec, int *nsec, int *kind, int *bar, int *len);
int gl_tide_kind_at(int s);
int gl_tide_bars_at(int s);
extern const char *const GL_TIDE_KIND[8];
float gl_space_now(int k);
/* audition: the noir songs' room and echo: the room's decay time and the echo's feedback as shares
 * of the usual (0.38, 0.62; 1, 1 the old long ones); from the next gl_piece */
void gl_room_noir(float room, float echo);
void gl_phrase_style(int st);         /* audition: the noir side's phrasing, 0 A (as first written), 1 B (after the listening notes) */          /* audition only (the instrument page): these channels stay under MIDI, their sequencers resting */
void gl_cho_share(int m);            /* audition only: one chorus for the Strings and the Kit (1 Juno, 2 the Kit's) */
void gl_rv_drift(int mode);          /* audition only: the Hall's lines drifting (1 grow/shrink, 2 diverge, 3 grow, 4 shrink) */
void gl_rv_shim_ratio(float r);      /* audition only: the Shimmer's interval as a ratio (below 1: down; 0 its own) */
void gl_rv_fx(int mode);              /* audition only: odd tails (1 crush growing, 2 crush per pass, 3/4 endless rise/fall, 5 ring arp, 6 ring contrary, 7/8 Fibonacci ring up/down) */
void gl_rv_cut(int bits);            /* audition only: 1 the Hall on four lines, 2 one damping band */
void gl_drop_cap(int n);             /* audition only: at most n notes and hits start in a step (0 off) */
uint32_t gl_thin_dropped(void);       /* audition only: how many the thinning let go */
void gl_thin(int cap, float thr);    /* audition only: past cap starts in a step, the least important (position x role x velocity < thr) go */
void gl_str_thin(int mask);          /* audition only: Strings thinned (bits: 1 no chorus, 2 one filter, 4 two poles, 8 one saw, 16 two notes) */
void gl_song_drums(int style);       /* song mode's drums leaving: 0 fade, 1 longer, 2 peel, 3 peel + a fill into the reverb */
void gl_kit_solo(int lanes);          /* audition only: the kit's lanes to play (bits; 0 all) */
void gl_kit_color(int method);        /* audition only: the Kit's colour (0 none, 1 comb, 2 chord resonators, 3 through-zero FM, 4 soggy, 5 metal, 6 soggy + Chebyshev: the default, 7 a comb's soggy + Chebyshev on the low end) */
void gl_kit_cache_stats(uint32_t *hits, uint32_t *miss);   /* audition only: the kick cache's hits */
/* the CPU options, switchable while it plays: the space mixed into the voices at 24 kHz (one
 * rate converter for both) and the control layer every ctl_n milliseconds (1 as before, 2 halves
 * its cost) */
void gl_cpu_opts(int space24, int ctl_n);
/* the load governor's state: the voice cap (half-voices) and how lean it has gone (0 none, 1 the
 * CPU options, 2 and the four-line Hall and cheaper Kit) */
void gl_gov_state(int *cap, int *lean);
/* the governor's very last resort: the channels it has faded out (bits), until the load has room */
int gl_ducked(void);
void gl_duck_reset(void);              /* (the hardware test: each reading from the same place) */
/* the light Strings (at 12 kHz) and Super (its outer saws plain): mode 2 always (the default,
 * GL_LITE_DEFAULT), 1 never (the full ones, kept), 0 only for a patch past the CPU budget.
 * gl_lite_mask: the channels playing light now */
void gl_lite(int mode);
int gl_lite_mask(void);
int gl_lite_default(void);              /* the mode this build starts in */
/* the presets' CPU rules, as gl_piece loads a piece (on by default; GL_KIT_RULE=0 or gl_kit_rule(0)
 * for the pieces as written): a Kit keeps only light company, and a piece with a heavy voice is
 * not on the Hall */
void gl_kit_rule(int on);
/* Performance effects (audition, glag_new.h; every one at 0 leaves the engine as it was):
 *   GF_ANSWER v      a Kit or Lite Kit fill is answered a beat later by the plucked voices (v: how often)
 *   GF_MEMORY v      each channel reuses its decision of a bar before, with this probability (Deja Vu)
 *   GF_BREATHE v     importance thinning: past the first start in a step, only what matters plays
 *   GF_HOLD_ECHO 0|1 the tape echo's loop held (nothing in, nothing lost); notes go on
 *   GF_HOLD_VERB 0|1 the reverb held forever, nothing new into it; notes go on
 *   GF_HEAD 0..3     the echo as a read head: 1 repeat the last echo-length slice, 2 tape stop,
 *                    3 reverse (0 off: back to the live echo over 25 ms)
 *   GF_SYMBIOSIS v   the channels' macro glides pull toward each other's phase
 *   GF_REGISTER v    register as a coordinate: low registers darker, high ones brighter */
enum gl_perf { GF_ANSWER, GF_MEMORY, GF_BREATHE, GF_HOLD_ECHO, GF_HOLD_VERB, GF_HEAD, GF_SYMBIOSIS, GF_REGISTER };
void gl_perform(int what, float v);
/* what each voice costs, in half-points of the CPU (the M4 model; hardware runs about 1.3x), and
 * a patch's estimated load in model points: 49.5 for the space and output, the voices, the reverb,
 * the reverse swell, and the extras the caller names (distortion, a Playworld texture 1-4, pump) */
extern const uint8_t GL_VCOST[];
float gl_cost_est(const gl_patch *p, int dist, int tex, int pump);
/* a test build (GL_PROF): each block's cost by part against a cycle counter, and flash-cache misses.
 * Parts: control, voices (but the Kit), the Kit, the space, the rest. Averages and worsts since the
 * last take, in counter units per block */
void gl_prof_hook(volatile const uint32_t *cycles, volatile const uint32_t *misses);
void gl_prof_take(uint32_t avg[5], uint32_t worst[5], uint32_t *miss_avg, uint32_t *miss_worst, uint32_t *blocks);
/* The hardware test's checks (glag_test.h), since the last take: how full the blocks ran (gl_load's
 * shares: the device only), what came out (the render's own output, before the firmware's VOL)
 * and the engine's counts. gl_check_take copies them and starts again. */
typedef struct {
	uint32_t loads, over90, over100, over150, over200;  /* gl_load calls; blocks past 90/100/150/200 % */
	float load_sum, load_worst;
	int lean_max, cap_min;                 /* the governor: its leanest step, its lowest voice cap */
	uint32_t ducks;                        /* channels the governor faded out (its very last resort) */
	int duck_max;                          /* the most out at once */
	uint32_t blocks, samples;              /* rendered */
	uint32_t clips, jumps, jump_max, peak; /* samples at the rail (|s| >= 32500); clicks (a lone step > 2000), the biggest one's L² + R² */
	uint32_t silent_max, nan;              /* the longest run of silent blocks (|s| < 8); blocks with a NaN */
	float dc[2], ms;                       /* sums of each block's mean (L, R) and mean square */
	uint32_t steps, step_notes_max, notes, steals;
	float beats;                           /* beats the tempo asked for, over the samples rendered */
	float ig_min;                          /* the reverb's input guard at its lowest (1: never stepped in) */
	uint32_t cyc;                          /* counter units the checks themselves took */
} gl_check_t;
void gl_check_take(gl_check_t *o);
uint32_t gl_check_cost(void);             /* the last block's checks, in counter units (left out of its load) */
void gl_kit_cheap(int bits);          /* audition only: cheaper Kit (1 the Nord at the kit's rate, smoothed jumps; 2 the drum's burst drawn as read; 4 the canvas kick from a cache) */
void gl_kit_col_half(int on);         /* audition: the Kit's chord resonators at 8 kHz (1) or 16 (0, as before) */
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
/* the scope: what is playing, for the emulator's track view (written only while gl_scope_on;
 * reads nothing back, so it can't change the sound). The layout is read by offset in the page. */
typedef struct {
	float t, bpm, beat, arc, space, duck, filt, comp, peak;               /* 0 .. 32 */
	float reverb, delay, filter, motion, autof, tex_lvl, pump;            /* 36 .. 60 */
	uint8_t chord, root, mode, rv, echo, flags, dist_type, dist_amt;      /* 64 */
	uint8_t dist_post, tex, song, kind, ending, key_off, cap, shim;       /* 72 */
	uint32_t notes;                                                       /* 80 */
	struct {                                                              /* 84, 60 bytes each */
		float lvl, wlvl, wact, act, mac, tone, chr, len, echo, verb, reg, pan, glow;
		int32_t note;
		uint8_t voice, pad[3];
	} ch[GL_NCH];
} gl_scope_t;
extern gl_scope_t gl_scope;
extern uint8_t gl_scope_on;

/* ---- pieces ---- */
#ifdef GL_DEMO_PIECES
#define GL_NPIECES 155                  /* an audition build: a hundred and two more (five of them labs), fourteen on the light voices, thirteen late rooms, ten space pieces */
#elif defined(GL_TEST_PIECES)
#define GL_NPIECES 20                   /* the CPU test build: the sixteen and the four heaviest auditions */
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
