/* SPDX-License-Identifier: MIT
 * Glag engine (glag_engine.h). One compilation unit; build it at -O2.
 *
 * Cost model (nRF52840, 64 MHz, 48 kHz: ~1333 cycles per stereo frame):
 *   - oscillators read a 1024-point sine table in RAM, phases are uint32 (wrap for free);
 *   - envelopes, indices and pitches are computed once per 24-sample sub-block and
 *     interpolated linearly inside it;
 *   - the echo runs at half rate and both delays store int16;
 *   - no double, no libm in the audio path.
 */
#include "glag_engine.h"
#include <string.h>

#define BARRIER() __asm__ volatile("" ::: "memory")

/* ================================================================ fast math */

/* 4096 points, read without interpolation: the error is harmonics ~60 dB down, and it is
 * three instructions instead of ten (16 KB of RAM well spent on this CPU). */
#define TBL 4096
#define TBL_SHIFT 20
static float SIN[TBL + 1];

static inline float sin_u(uint32_t ph) { return SIN[ph >> TBL_SHIFT]; }   /* full turn = 2^32 */

/* 2^x, |x| < 60. Round-to-nearest split, 5th-order series on [-0.5, 0.5]: exact at 0, so
 * coefficients just below 1 come out right (a decay must not stop or grow). */
static float fexp2(float x)
{
	int i = (int)(x + (x >= 0.0f ? 0.5f : -0.5f));
	float y = (x - (float)i) * 0.69314718f;
	float p = 1.0f + y * (1.0f + y * (0.5f + y * (0.16666667f + y * (0.04166667f + y * 0.00833333f))));
	union { float f; int32_t i; } u;
	u.f = p;
	u.i += i * (1 << 23);
	return u.f;
}
static inline float mtof(float m) { return 440.0f * fexp2((m - 69.0f) * (1.0f / 12.0f)); }
/* The instruments run at half rate (24 kHz) and are brought back up to 48 kHz after the mix:
 * nearly half the cost of every voice, for nothing above 12 kHz (heard in round 2 of the
 * reverb audition; only a fully buzzing Shimmer aliased noticeably, dry). */
#ifndef VOICE_HALF
#define VOICE_HALF 1                     /* 0: full rate, for measuring the half rate's cost on a computer */
#endif
#define VSUB (GL_SUB >> VOICE_HALF)
#define VSR (GL_SR / (float)(1 << VOICE_HALF))
static inline uint32_t hz_inc(float hz) { return (uint32_t)(hz * (89478.485f * (float)(1 << VOICE_HALF))); }
static inline uint32_t hz_inc48(float hz) { return (uint32_t)(hz * 89478.485f); }   /* 2^32 / 48000 */
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
/* coefficient that falls by 60 dB over t seconds, applied every `dt` seconds */
static inline float dcoef(float t, float dt) { return fexp2(-9.9658f * dt / t); }
static inline float fmodf_pos(float x, float m) { x -= (float)(int)(x / m) * m; return x < 0.0f ? x + m : x; }
/* log2(x), x > 0: exponent from the bits, a cubic for the mantissa (control rate only) */
static float flog2(float x)
{
	union { float f; int32_t i; } u;
	u.f = x;
	float e = (float)(((u.i >> 23) & 255) - 128);
	u.i = (u.i & 0x007FFFFF) | 0x3F800000;                 /* mantissa in [1, 2) */
	float m = u.f;
	return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;   /* exponent - 128 + (log2 m + 1) */
}
/* lo..hi exponentially */
static inline float expmap(float v, float lo, float hi) { return lo * fexp2(v * flog2(hi / lo)); }
/* a phase increment per sub-block for `hz`, at the current speed */
static inline uint32_t sub_inc(float hz, float spd) { return hz_inc48(hz * spd) * GL_SUB; }

/* ================================================================ random */

static uint32_t rng = 0x9E3779B9u;
static inline uint32_t rnd32(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}
static inline float rnd(void) { return (float)(rnd32() >> 8) * (1.0f / 16777216.0f); }
static inline int rndi(int n) { return (int)((rnd32() >> 8) % (uint32_t)n); }

/* ================================================================ names, steps */

const char *const GL_VOICE_NAME[GV_COUNT] = {"Pad", "Bells", "Bass", "Piano", "Marimba",
                                             "Drone", "Kalimba", "Flute", "Arp", "Swell",
                                             "EP", "Gong", "Pizz", "Shimmer", "Tails", "Pulse",
                                             "Strings", "Glow", "Seq", "Super",
                                             "Ribbon", "Funk", "Harp", "Harpsichord", "Kit"};
const char *const GL_MODE_NAME[GL_NMODES] = {"Ionian", "Dorian", "Phrygian", "Lydian",
                                             "Mixolydian", "Aeolian", "Pent maj", "Pent min"};
static const uint16_t MODE_POOL[GL_NMODES] = {
	0xAB5, /* 0 2 4 5 7 9 11 */  0x6AD, /* 0 2 3 5 7 9 10 */  0x5AB, /* 0 1 3 5 7 8 10 */
	0xAD5, /* 0 2 4 6 7 9 11 */  0x6B5, /* 0 2 4 5 7 9 10 */  0x5AD, /* 0 2 3 5 7 8 10 */
	0x295, /* 0 2 4 7 9 */       0x4A9, /* 0 3 5 7 10 */
};

int gl_cp_steps(int cp) { return cp == CP_REGISTER ? 5 : 0; }
int gl_gp_steps(int gp) { return gp == GP_ROOT ? 12 : gp == GP_MODE ? GL_NMODES : 0; }
int gl_cp_centred(int cp) { return cp == CP_PAN; }
int gl_gp_centred(int gp) { return gp == GP_FILTER; }
float gl_tempo_bpm(float v) { return 20.0f + 100.0f * clampf(v, 0.0f, 1.0f); }
static int stepped(float v, int n) { int k = (int)(v * (float)(n - 1) + 0.5f); return k < 0 ? 0 : k >= n ? n - 1 : k; }

int gl_scale(int mode, int *pcs)
{
	int n = 0;
	uint16_t pool = MODE_POOL[(unsigned)mode < GL_NMODES ? mode : 0];
	for (int i = 0; i < 12; i++)
		if (pool & (1u << i))
			pcs[n++] = i;
	return n;
}

/* ================================================================ state */

volatile float gl_glow[GL_NCH];
volatile float gl_beat;
volatile uint8_t gl_chord_root;
volatile uint8_t gl_playing = 1, gl_frozen;
volatile float gl_speed = 1.0f;
volatile float gl_peak;

/* one oscillator voice: 2-op FM (or 1-op with feedback, or two carriers for the bass) */
typedef struct {
	uint32_t pc, pm, ic, im;       /* carrier / modulator phase and increment */
	float amp, ainc;               /* per-sample amplitude and its ramp */
	float idx, iinc;               /* per-sample modulation index (phase units) and ramp */
	float env, menv;               /* envelope states, per sub-block */
	float note, vel, ratio;
	uint8_t side;                  /* which half of the channel's stereo pair it plays in */
	float hold;                    /* flute: seconds left at full breath */
	float fb;                      /* flute: feedback memory */
	uint8_t stage;                 /* 0 idle, 1 attack, 2 decay / sustain, 3 release */
	/* the newer voices: a second carrier (swell) or modulator (EP's tine), its index, the
	 * seconds since the note began (gong, shimmer, tails) and a pitch bend in semitones */
	uint32_t pc2, pm2, ic2, im2;
	float idx2, iinc2, menv2, t, bend;
	/* the analog voices (strings, glow, seq, super): more saw phases, and a filter's states */
	uint32_t pc3, pc4, ic3, ic4;
	float f1, f2, fk, fq;
	uint8_t mnote;                 /* the MIDI note it is playing, + 1 (0: none) */
} nvoice;
enum { ST_IDLE, ST_ATT, ST_DEC, ST_REL };

#define NV 4
/* plucked strings (Karplus-Strong), carved out of one buffer per channel: the kalimba's 3 of
 * 1024 samples, the harp's and the harpsichord's 8 of 384 (down to B1) */
#define KS_N 8
#define KS_BUF 3072
#define BELL_MAX 29
static const uint8_t BELL_LEN[4] = {13, 17, 23, 29};
static const float PAD_FACTOR[NV] = {1.0f, 1.31f, 1.62f, 1.93f};   /* unequal breaths */
/* Voices that spread their notes across two sides of the channel (pad, bells);
 * the rest are mono (their channel pan places them), which saves a buffer and half the mixing. SPREAD: how far each side sits
 * from the channel's pan. */
static const uint8_t STEREO[GV_COUNT] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1,
                                        0, 0, 1, 1, 0};
static const float SPREAD[GV_COUNT] = {0.3f, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.25f, 0.0f, 0.25f,
                                       0.35f, 0.0f, 0.0f, 0.0f, 0.3f, 0.3f, 0.2f, 0.35f, 0.0f, 0.0f, 0.4f,
                                       0.0f, 0.0f, 0.35f, 0.25f, 0.0f};

/* the kit (glag_kit.h): a snare (also the roll's), and each sound's state */
typedef struct { float x1, y1, x2, y2, e1, e2, drop, dk, b, bk, n, nk, hp, l1, l2, lpk, nl, drive, lo; } kit_snare;
typedef struct {
	float k_x[5], k_y[5], k_e[5], k_amp[5], k_dec[5], k_glide, k_gk, k_att, k_ai, k_nz, k_nzk, k_lp, k_lpk;
	kit_snare sn;
	uint32_t h_ph[6];
	float h_a, h_k, h_s0, h_s1, h_t, h_len, h_f, h_grit, h_lpk, h_lp, h_lpe, h_fc0, h_fenv, h_lo, h_bp, h_hp, h_l2, h_l3;
	float ha_v[2][2], hb_v[2][2], h_ad, h_c, h_s;
	uint32_t d_ic, d_im, d_pc, d_pm;
	float d_index, d_ik, d_a, d_k, d_fb, d_m1, d_m2;
	int d_grit;
	float t_len, t_t, t_a, t_prob, t_bp, t_fc, t_prox, t_w0, t_w1, t_ph0, t_c, t_lo, t_b, t_hp, t_r1, t_r2;
	float n_f1, n_sweep, n_fm, n_md, n_menv, n_cut, n_a, n_ak, n_att, n_p1, n_p2, n_lo1, n_bp1, n_lo2, n_bp2, n_dc;
	uint8_t r_n, r_i, r_mode, r_split;
	int8_t r_dir;
	float r_gap, r_accel, r_t, r_vel, r_hold, r_q, r_semis, r_acc, r_held;
	/* hits waiting a fraction of a step: off-grid slots, swing, triplets */
	float q_t[8], q_v[8];
	uint8_t q_l[8];
	/* the rotation (the macro's wildest trick): kick -> snare -> hat -> kick, a turn each segment */
	uint8_t rot_r, rot_left, rot_seg, rot_i;
	kit_snare rs;
} kit_state;

typedef struct {
	uint8_t voice, want, fading;
	float fade;                    /* 0..1 switch fade */
	float lvl, lvl_inc;            /* level, interpolated */
	nvoice v[NV];
	uint8_t vnext;
	int last_note;
	/* pad */
	float cyc[NV], gap[NV];
	/* bells */
	int8_t loop[4][BELL_MAX];
	uint8_t lpos[4];
	/* bass / drone */
	float glide_note, target_note, swell;
	/* arp */
	uint8_t arp_i, arp_dir;
	/* kalimba */
	int16_t ks[KS_BUF];
	uint16_t ks_len[KS_N], ks_i[KS_N], ks_fill[KS_N];
	float ks_g[KS_N], ks_a[KS_N], ks_ea[KS_N], ks_elp[KS_N], ks_vel[KS_N];
	float ks_env[KS_N], ks_kd[KS_N];   /* an estimate of the string's level, for stopping it */
	/* harp, harpsichord: a tuning all-pass in the loop (its coefficient, memories), and how long
	 * until the damper falls (harpsichord) */
	float ks_c[KS_N], ks_x1[KS_N], ks_y1[KS_N], ks_hold[KS_N];
	uint8_t ks_next;
	/* harp: a rolled chord or a glissando, its notes waiting */
	int8_t hq[16];
	uint8_t hq_n, hq_i;
	float hq_t, hq_dt, hq_vel;
	float glow;
	uint32_t seen_chord;
	uint32_t lfo1, lfo2;           /* drone: index swell and breath */
	/* shimmer: the octave echo waiting to sound */
	float echo_at, echo_note, echo_vel;
	/* pulse: kick (phase, pitch, its fall's end, level, decay), shaker, click; a swung hit waiting */
	float pk_ph, pk_f, pk_fend, pk_a, pk_k, ps_a, ps_k, ps_x, pc_ph, pc_inc, pc_a;
	float ps_h, ps_y, ps_z, sn_m, sn_l;  /* the shaker's top, rolled off; the snare noise's filters */
	uint32_t pn;                   /* its own noise (xorshift): not the shared generator */
	float pc_k, sn_a, sn_k;        /* the click's decay; the snare's noise (grid mode) */
	float pend_t;
	uint8_t pend;                  /* (the mask fits: up to 127) */
	/* bells: a note waiting to sound, per loop */
	float bp_t[4], bp_note[4], bp_vel[4], bp_ratio[4];
	uint8_t bp_on;
	kit_state kt;                  /* the kit (glag_kit.h) */
} chan;

static gl_patch P;                         /* the patch the render plays */
static gl_patch post_buf[2];
static volatile uint32_t post_seq, post_seen = 0xFFFFFFFFu;

static volatile uint8_t cmd_play = 1, cmd_freeze, cmd_slow;
static volatile uint32_t cmd_next, cmd_panic, cmd_fresh, cmd_fresh_seed, cmd_crank;
static uint32_t seen_crank, notes_fired;     /* the crank; notes begun (for it to find the next) */
static uint32_t seen_fresh;
static uint32_t seen_next, seen_panic;

/* harmony */
static int scale_pc[12], scale_n;          /* absolute pitch classes, ascending from the root */
static int root_pc, mode_cur = -1, root_cur = -1;
static int chord_deg, chord_pc[4], chord_n, chord_moves;
static uint32_t chord_count;
static uint16_t scale_mask, chord_mask;     /* bit pc: in the scale / in the chord */
static float chord_timer;
static uint8_t chord_pending;

/* time */
static float step_acc;                     /* 16ths */
/* Playworld's hooks (glag_play.h): per channel level and activity from the form, the pump, the
 * tape's ping-pong, the macros' shape, the key's travel */
static float w_lvl[GL_NCH] = {1.0f, 1.0f, 1.0f, 1.0f}, w_act[GL_NCH] = {1.0f, 1.0f, 1.0f, 1.0f};
static float pump_env, pump_tgt, sp_duck = 1.0f;
static float pump_prev[GL_NCH] = {1.0f, 1.0f, 1.0f, 1.0f};   /* each channel's duck last block */
static int w_pingpong, key_off, mode_swap, form_fill;
static float form_space = 1.0f, form_filt = -1.0f;
/* the master compressor: how much (0 off), its detector, the gain now and per sample */
static float comp_amt, comp_env, comp_g = 1.0f;
static uint8_t mac_shape[GL_NCH];
static uint32_t step_n;
static float speed = 1.0f, arc;
static float pbend = 1.0f;                 /* pitch only (Playworld's pump bends the notes as it ducks) */
static uint32_t lfo_vib, lfo_arc1, lfo_arc2, lfo_trem;
static uint8_t playing = 1, frozen, slowing;
static float freeze_in = 1.0f;             /* reverb input gain, ramps to 0 while frozen */
static float crank_open;                   /* a crank while frozen: the space open to it a moment */
static int tp_hold;                        /* frozen: the tape loop holds what it has */

/* ================================================================ harmony */

static void harmony_update(void)
{
	int mode = stepped(P.g[GP_MODE], GL_NMODES);
	int root = (stepped(P.g[GP_ROOT], 12) + key_off + 24) % 12;
	if (mode_swap) {
		/* the relative major / minor: the same notes, another home. Where each mode sits in its
		 * major scale (Ionian .. Aeolian; the pentatonics as major and minor) */
		static const int8_t AT[8] = {0, 2, 4, 5, 7, 9, 0, 9};
		int maj = (root - AT[mode] + 12) % 12;
		if (mode == 6) { mode = 7; root = (maj + 9) % 12; }
		else if (mode == 7) { mode = 6; root = maj; }
		else if (mode == 0 || mode == 3 || mode == 4) { mode = 5; root = (maj + 9) % 12; }
		else { mode = 0; root = maj; }
	}
	if (mode == mode_cur && root == root_cur)
		return;
	int rel[12];
	scale_n = gl_scale(mode, rel);
	scale_mask = 0;
	for (int i = 0; i < scale_n; i++) {
		scale_pc[i] = (rel[i] + root) % 12;
		scale_mask |= (uint16_t)(1u << scale_pc[i]);
	}
	mode_cur = mode;
	root_cur = root;
	root_pc = root;
	if (chord_deg >= scale_n)
		chord_deg = 0;
	chord_pending = 1;                         /* rebuild the chord on the new scale now */
}

static void chord_build(void)
{
	chord_n = (scale_n >= 7 && rnd() < 0.6f) ? 4 : 3;
	chord_mask = 0;
	for (int k = 0; k < chord_n; k++) {
		chord_pc[k] = scale_pc[(chord_deg + 2 * k) % scale_n];
		chord_mask |= (uint16_t)(1u << chord_pc[k]);
	}
	gl_chord_root = (uint8_t)chord_pc[0];
	chord_count++;
}

static void chord_next(void)
{
	/* a gentle walk: IV, V, vi, ii mostly, and home again after a few moves */
	static const int8_t MOVE[6] = {3, 4, 5, 1, -2, 2};
	static const uint8_t W[6] = {30, 22, 20, 14, 7, 7};
	if (chord_moves >= 3 && rnd() < 0.5f) {
		chord_deg = 0;
		chord_moves = 0;
	} else {
		int r = rndi(100), k = 0, acc = W[0];
		while (r >= acc && k < 5)
			acc += W[++k];
		chord_deg = (chord_deg + MOVE[k] + scale_n * 2) % scale_n;
		chord_moves = chord_deg ? chord_moves + 1 : 0;
	}
	chord_build();
}

static inline int in_chord(int pc) { return (chord_mask >> pc) & 1; }
static inline int in_scale(int pc) { return (scale_mask >> pc) & 1; }

/* a melodic note in lo..hi: a chord tone with probability cb, else any scale tone;
 * mostly near the last note, never the same note twice */
static int pick_note(chan *c, int lo, int hi, float cb)
{
	int cand[48], n = 0, want_chord = rnd() < cb;
	int near = c->last_note >= lo && c->last_note <= hi && rnd() < 0.7f;
	for (int m = lo; m <= hi && n < 48; m++) {
		int pc = m % 12;
		if (!(want_chord ? in_chord(pc) : in_scale(pc)) || m == c->last_note)
			continue;
		if (near && (m - c->last_note > 7 || c->last_note - m > 7))
			continue;
		cand[n++] = m;
	}
	if (!n) {
		for (int m = lo; m <= hi && n < 48; m++)
			if (in_scale(m % 12))
				cand[n++] = m;
		if (!n)
			return lo;
	}
	int m = cand[rndi(n)];
	c->last_note = m;
	return m;
}

/* pitch class pc placed nearest to `from`, inside lo..hi (lo >= 0; hi - lo >= 11) */
static int place(int pc, float from, int lo, int hi)
{
	int m = lo + (pc - lo % 12 + 12) % 12;          /* the lowest pc in range */
	int f = (int)from;
	while (m + 12 <= hi && (m + 12 - f) * (m + 12 - f) <= (m - f) * (m - f))
		m += 12;
	return m;
}

/* ================================================================ per-voice ranges */

/* home note of each voice; REGISTER moves it by octaves (-2..+2) */
/* plucked or struck (bright into the reverb), rather than held */
static const uint8_t PLUCK[GV_COUNT] = {0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0,
                                       0, 0, 1, 1, 1};
#ifndef GLOW_HOME
#define GLOW_HOME 69                       /* glow: A4, as it began (its top is cut instead) */
#endif
static const uint8_t HOME[GV_COUNT] = {57, 72, 38, 64, 72, 45, 72, 72, 67, 57, 60, 33, 67, 79, 64, 36, 57, GLOW_HOME, 55, 57,
                                      67, 40, 62, 60, 36};

/* ---- the channel macro: a slow random glide pushing each voice's own controls around ----
 * Per voice, how far the macro moves:   tone  char  trem  oct   len   act */
static const uint8_t MW[GV_COUNT][6] = {
	{60, 50, 40,  0, 30, 20},   /* pad: brightness, detune, breathing tremolo */
	{50, 70,  0, 50, 30, 30},   /* bells: metal, octave leaps */
	{40, 60,  0, 30, 30, 20},   /* bass: glide and fifths, the odd octave */
	{60, 60, 20, 40, 40, 30},   /* piano: tine, octaves */
	{50, 80,  0, 50, 30, 40},   /* marimba: ratio, octaves */
	{80, 30, 30,  0,  0, 30},   /* drone: the index opens wider, interval shifts */
	{60, 60,  0, 50, 40, 40},   /* kalimba */
	{50, 80, 30, 30, 30, 20},   /* flute: vibrato */
	{50, 60, 20, 60, 40, 30},   /* arp: pattern, octave range */
	{60, 70, 30,  0, 30, 20},   /* swell: detune, vibrato */
	{60, 60, 60, 30, 30, 30},   /* ep: suitcase tremolo, tine */
	{50, 80,  0, 20, 30, 20},   /* gong: metal */
	{50, 60,  0, 60, 40, 40},   /* pizz: octave jumps */
	{50, 90, 30, 30, 30, 20},   /* shimmer: buzz */
	{50, 90,  0, 40, 40, 30},   /* tails: tail shape */
	{50, 60,  0,  0, 40, 50},   /* pulse: swing, brightness, hits */
	{80, 80, 30,  0, 40, 20},   /* strings: the filter swelling open and shut, the ensemble thickening */
	{60, 95, 50, 30, 40, 30},   /* glow: sine melting into triangle and back, a breathing tremolo */
	{95, 70,  0, 40, 70, 40},   /* seq: cutoff and decay swept wide (acid), the pattern turning */
	{85, 95, 20,  0, 30, 20},   /* super: the saws' spread breathing, the filter sweeping */
	{70, 70, 40, 30, 50, 30},   /* ribbon: the brass opening, brass to reed, longer phrases */
	{70, 80,  0, 30, 40, 60},   /* funk: fingers to slap, busier, more off the beat */
	{40, 90,  0, 40, 50, 40},   /* harp: plucks, rolled chords, glissandi */
	{40, 70,  0, 40, 50, 50},   /* harpsichord: the stops, the figures */
	{50, 90,  0,  0, 50, 50},   /* kit: the split (core -> tissue and Nord -> rolls), decays, hits */
};
/* arp mode (glag_play.h): whether a channel is being played by it, and the held voices' short
 * envelope while it is */
static int arp_live(int ch);
static int midi_live(int ch);
static int arp_held(int voice);
static void arp_held_control(chan *c, int ch, float dt, float tone, float len);
/* audition only: another way to make a voice (0: as it is), to compare methods by ear */
/* the chosen ways (picked by ear from three each): Juno PWM strings, warm Glow, subtractive
 * Ribbon, the noise-plucked Harp */
static uint8_t alt_m[GV_COUNT] = {[GV_STRINGS] = 2, [GV_GLOW] = 2, [GV_RIBBON] = 1, [GV_HARP] = 1};
void gl_alt(int voice, int method)
{
	if ((unsigned)voice < GV_COUNT)
		alt_m[voice] = (uint8_t)method;
}
static float mac_depth[GL_NCH], mac_period[GL_NCH] = {8.0f, 8.0f, 8.0f, 8.0f};
static float mac_user[GL_NCH];                  /* as set; song mode's randomness may add to it */
static float mac_pmul[GL_NCH] = {1.0f, 1.0f, 1.0f, 1.0f};   /* song mode: each macro's pace varied */
static float mac_ph[GL_NCH][2], mac_from[GL_NCH][2], mac_to[GL_NCH][2], mac_u[GL_NCH][2];
static float mac_trem_ph[GL_NCH];
static gl_patch Pb;                    /* the patch as posted, before the macro moves it */
void gl_macro(int ch, float depth, float period_s)
{
	if ((unsigned)ch < GL_NCH) {
		mac_depth[ch] = depth;
		mac_user[ch] = depth;
		mac_period[ch] = period_s;
	}
}
static int reg_base(const chan *c, int ch)
{
	int jump = 0;
	float d = mac_depth[ch];
	float pj = d * (1.0f + d) * MW[c->voice][3] * 0.006f;   /* up to ~0.7 of notes leap at full */
	if (d > 0.0f && rnd() < pj) {
		jump = rnd() < 0.6f ? 12 : -12;
		if (d > 0.7f && rnd() < 0.4f)
			jump *= 2;                                       /* two octaves, near full */
	}
	return HOME[c->voice] + 12 * (stepped(P.ch[ch][CP_REGISTER], 5) - 2) + jump;
}

static float eff_activity(int ch) { return clampf(P.ch[ch][CP_ACTIVITY] * w_act[ch] * (1.0f + 0.6f * arc), 0.0f, 1.0f); }
static float eff_tone(int ch) { return clampf(P.ch[ch][CP_TONE] + 0.15f * arc, 0.0f, 1.0f); }

/* ================================================================ note voices */

#define IDX_SCALE 683565275.6f            /* radians -> phase units */

static int chan_of(const nvoice *v);
static float ivl_note(int ch, float note);
static void nv_trigger(nvoice *v, float note, float vel, float ratio, int side)
{
	v->note = ivl_note(chan_of(v), note);
	v->mnote = 0;                  /* (MIDI tags its own notes after this) */
	notes_fired++;
	v->vel = vel;
	v->ratio = ratio;
	v->side = (uint8_t)(side & 1);
	v->stage = ST_ATT;
	v->menv = 1.0f;
	v->menv2 = 1.0f;
	v->t = 0.0f;
	v->bend = 0.0f;
	/* the index starts from nothing: a higher note must not inherit a lower one's brightness
	 * (for its first millisecond it would alias, a grinding edge on every strike) */
	v->idx = v->iinc = v->idx2 = v->iinc2 = 0.0f;
	/* and its pitch is the new note's from the first sample (the cap above reads it) */
	float hz = mtof(note) * speed * pbend;
	v->ic = hz_inc(hz);
	v->im = hz_inc(hz * ratio);
	/* the attack starts from where the amplitude is: no click on a retrigger */
	v->env = v->amp > 0.0f ? v->amp / (vel > 0.01f ? vel : 0.01f) : 0.0f;
	if (v->env > 1.0f)
		v->env = 1.0f;
	if (v->env < 0.0f)
		v->env = 0.0f;
}

/* ---- the voice budget: at most FM_CAP oscillator voices sound at once, across channels.
 * A new note past the cap fades out the quietest decaying note anywhere (over 1 ms); the
 * pad, the drone and the bass are never taken, but they count. This bounds the worst case
 * whatever four voices are chosen. */
/* Counted in half-voices: a 2-op voice is 2, the three-oscillator swell and EP are 3. The
 * distortion, when it is on, takes two voices' worth out of the budget. */
#ifndef FM_CAP
#define FM_CAP 18
#endif
#ifndef FM_DIST_CUT
#define FM_DIST_CUT 4
#endif
static int fm_cap = FM_CAP, fm_dist = FM_CAP;   /* the cap now; the cap the distortion leaves */
/* The load governor: the audio thread reports each block's share of its time (gl_load); past
 * 88 % the cap comes down a voice at a time (the quietest decaying notes give way), and after a
 * second under 72 % it goes back up. A safety net under the worst cases, never heard otherwise. */
static int gov_cap = FM_CAP, gov_cool;
void gl_load(float frac)
{
	if (frac > 0.88f) {
		gov_cap = gov_cap - 2 > 8 ? gov_cap - 2 : 8;
		gov_cool = 0;
	} else if (frac < 0.72f && gov_cap < FM_CAP && ++gov_cool >= (int)(GL_SR / 240.0f)) {
		gov_cool = 0;
		gov_cap += 2;
	}
	fm_cap = fm_dist < gov_cap ? fm_dist : gov_cap;
}
static chan CH[GL_NCH];
static int chan_of(const nvoice *v)
{
	for (int c = 0; c < GL_NCH; c++)
		if (v >= CH[c].v && v < CH[c].v + NV)
			return c;
	return -1;
}
/* the channel's interval (GL_CHX_IVL): the note moved up that many steps of the scale, a third,
 * fifth, seventh or ninth above, so a channel can play the harmony line to another */
static float ivl_note(int ch, float note)
{
	static const int8_t STEPS[GL_NIVL] = {0, 2, 4, 6, 8};
	if (ch < 0 || scale_n <= 0)
		return note;
	int iv = (int)GL_CHX_IVL(P.chx[ch]);
	if (iv <= 0 || iv >= GL_NIVL)
		return note;
	int n = (int)(note + 0.5f);
	int pc = ((n - root_pc) % 12 + 12) % 12, k = 0;
	int rel[12];
	for (int i = 0; i < scale_n; i++)
		rel[i] = (scale_pc[i] - root_pc + 12) % 12;
	for (int i = 0; i < scale_n; i++)                /* its step in the scale (or the one below) */
		if (rel[i] <= pc)
			k = i;
	int st = scale_n == 7 ? STEPS[iv] : (STEPS[iv] * scale_n + 3) / 7;
	int at = k + st;
	int up = rel[at % scale_n] + 12 * (at / scale_n) - rel[k];
	return note + (float)up;
}
static int nslots(int voice)
{
	switch (voice) {
	case GV_PAD: return 3;
	case GV_BELLS: return 4;
	case GV_BASS: return 1;
	case GV_DRONE: return 2;
	case GV_KALIMBA: return 0;
	case GV_GONG: return 2;
	case GV_SHIMMER: return 4;
	case GV_PULSE: return 0;                  /* no FM voices: outside the budget */
	case GV_RIBBON: return alt_m[GV_RIBBON] == 3 ? 3 : 1;   /* one line (or a section's chord) */
	case GV_FUNK: return 1;
	case GV_HARP: return alt_m[GV_HARP] == 3 ? 4 : 0;
	case GV_HARPSI: return 0;                 /* strings: counted below */
	case GV_KIT: return 0;                    /* drums: outside the budget */
	default: return 3;
	}
}
static int weight(int voice)
{
	if (voice == GV_RIBBON && alt_m[GV_RIBBON] >= 2)
		return 2;                             /* FM brass */
	return voice == GV_SUPER || voice == GV_RIBBON ? 4 : voice == GV_SWELL || voice == GV_EP ? 3 :
	       voice == GV_GLOW ? 1 : 2;
}
static inline int sounding(const nvoice *v) { return v->stage != ST_IDLE || v->amp > 0.0001f; }
static int stealable(int voice)
{
	return voice == GV_BELLS || voice == GV_PIANO || voice == GV_MARIMBA || voice == GV_ARP ||
	       voice == GV_FLUTE || voice == GV_EP || voice == GV_GONG || voice == GV_PIZZ ||
	       voice == GV_SHIMMER || voice == GV_TAILS || voice == GV_GLOW || voice == GV_SEQ;
}
static int budget_used(void)
{
	int n = 0;
	for (int c = 0; c < GL_NCH; c++) {
		chan *ch = &CH[c];
		int ns = nslots(ch->voice), w = weight(ch->voice);
		for (int i = 0; i < ns; i++)
			if (sounding(&ch->v[i]))
				n += w;
		if (ch->voice == GV_KALIMBA)               /* a ringing string costs about a voice */
			for (int k = 0; k < 3; k++)
				n += ch->ks_len[k] ? 2 : 0;
		else if (ch->voice == GV_HARP || ch->voice == GV_HARPSI)   /* tuned strings: about half */
			for (int k = 0; k < KS_N; k++)
				n += ch->ks_len[k] ? 1 : 0;
	}
	return n;
}
/* a new note of weight w is about to sound in `keep`: fade the quietest stealable note out
 * if the budget would overflow */
static void make_room_w(const nvoice *keep, int w)
{
	int n = 0;
	nvoice *q = 0;
	float qa = 1e9f;
	for (int c = 0; c < GL_NCH; c++) {
		chan *ch = &CH[c];
		int ns = nslots(ch->voice), st = stealable(ch->voice), cw = weight(ch->voice);
		for (int i = 0; i < ns; i++) {
			nvoice *v = &ch->v[i];
			if (!sounding(v))
				continue;
			n += cw;
			if (st && v != keep && v->amp < qa) {
				qa = v->amp;
				q = v;
			}
		}
	}
	if (n + w > fm_cap && q) {
		q->stage = ST_IDLE;                    /* its amplitude ramps to 0 in the next ms */
		q->env = 0.0f;
	}
}
static void make_room(const nvoice *keep) { make_room_w(keep, 2); }

/* set this sub-block's ramps toward amplitude `a` and index `i`. A voice that has faded
 * is snapped to exactly zero, or float rounding would leave it rendering forever. */
/* An FM voice's partials reach about fc + (index + 1) fm (Carson). The voices run at 24 kHz,
 * so anything past 12 kHz would fold back down as aliasing (a grinding edge on high bells,
 * marimba, shimmer): cap each note's index so its partials stay under 10 kHz, with a sideband
 * to spare (Carson's rule leaves the next one only ~20 dB down). Low notes are never touched;
 * the highest, brightest ones lose only what would have aliased. */
#define HZ_PER_INC (1.0f / (89478.485f * (float)(1 << VOICE_HALF)))
#ifndef IDX_TOP
#define IDX_TOP (0.42f * VSR)                /* 10 kHz at half rate */
#endif
/* from the oscillators' increments (nv_trigger sets them for the new note at once, so a
 * strike never reads the last note's pitch); mult widens fm for a richer modulator */
static inline float idx_cap(const nvoice *v, float i, float mult)
{
	float fc = (float)v->ic * HZ_PER_INC, fm = (float)v->im * HZ_PER_INC * mult;
	if (fm < 1.0f)
		return i;
	float cap = ((IDX_TOP - fc) / fm - 2.0f) * IDX_SCALE;   /* Carson, plus a sideband to spare */
	cap = cap > 0.0f ? cap : 0.0f;
	return i < cap ? i : cap;
}

static inline void ramp(nvoice *v, float a, float i)
{
	i = idx_cap(v, i, 1.0f);
	if (a <= 0.0f && v->amp < 0.0002f && v->amp > -0.0002f)
		v->amp = v->ainc = 0.0f;
	else
		v->ainc = (a - v->amp) * (1.0f / VSUB);
	v->iinc = (i - v->idx) * (1.0f / VSUB);
}

static nvoice *alloc(chan *c, int n)
{
	nvoice *best = &c->v[0];
	for (int i = 0; i < n; i++)               /* an idle voice, else the quietest */
		if (c->v[i].stage == ST_IDLE)
			return &c->v[i];
	for (int i = 1; i < n; i++)
		if (c->v[i].amp < best->amp)
			best = &c->v[i];
	return best;
}

/* per sub-block: percussive envelope (attack `att` s, decay to -60 dB in `dec` s) */
static void nv_perc(nvoice *v, float att, float dec, float mdec, float imax, float dt)
{
	if (v->stage == ST_ATT) {
		v->env += dt / att;
		if (v->env >= 1.0f) {
			v->env = 1.0f;
			v->stage = ST_DEC;
		}
	} else if (v->stage == ST_DEC) {
		v->env *= dcoef(dec, dt);
		if (v->env < 0.0005f) {
			v->env = 0.0f;
			v->stage = ST_IDLE;
		}
	}
	v->menv *= dcoef(mdec, dt);
	float a = v->env * v->vel, i = clampf(imax * (0.15f + 0.85f * v->menv), 0.0f, 3.0f) * IDX_SCALE;
	ramp(v, a, i);
}

/* pitch for this sub-block (slowdown bends everything down together) */
static void nv_pitch(nvoice *v, float note)
{
	float hz = mtof(note) * speed * pbend;
	v->ic = hz_inc(hz);
	v->im = hz_inc(hz * v->ratio);
}

/* render one 2-op FM voice, adding into `out` (its side of the channel) */
static void nv_render(nvoice *v, float *out)
{
	uint32_t pc = v->pc, pm = v->pm, ic = v->ic, im = v->im;
	float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc;
	for (uint32_t i = 0; i < VSUB; i++) {
		float m = SIN[pm >> TBL_SHIFT] * idx;
		out[i] += sin_u(pc + (uint32_t)(int32_t)m) * amp;
		pm += im;
		pc += ic;
		amp += ai;
		idx += ii;
	}
	v->pc = pc; v->pm = pm; v->amp = amp; v->idx = idx;
}

/* swell: one modulator shared by two detuned carriers (pc and pc2) */
static void nv_render_dual(nvoice *v, float *out)
{
	uint32_t pc = v->pc, pc2 = v->pc2, pm = v->pm, ic = v->ic, ic2 = v->ic2, im = v->im;
	float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc;
	for (uint32_t i = 0; i < VSUB; i++) {
		uint32_t m = (uint32_t)(int32_t)(SIN[pm >> TBL_SHIFT] * idx);
		out[i] += (sin_u(pc + m) + sin_u(pc2 + m)) * amp;
		pm += im;
		pc += ic;
		pc2 += ic2;
		amp += ai;
		idx += ii;
	}
	v->pc = pc; v->pc2 = pc2; v->pm = pm; v->amp = amp; v->idx = idx;
}

/* EP: the body modulator (1:1) and the tine (14:1) both modulating one carrier */
static void nv_render_ep(nvoice *v, float *out)
{
	uint32_t pc = v->pc, pm = v->pm, pm2 = v->pm2, ic = v->ic, im = v->im, im2 = v->im2;
	float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc, idx2 = v->idx2, ii2 = v->iinc2;
	for (uint32_t i = 0; i < VSUB; i++) {
		float m = SIN[pm >> TBL_SHIFT] * idx + SIN[pm2 >> TBL_SHIFT] * idx2;
		out[i] += sin_u(pc + (uint32_t)(int32_t)m) * amp;
		pm += im;
		pm2 += im2;
		pc += ic;
		amp += ai;
		idx += ii;
		idx2 += ii2;
	}
	v->pc = pc; v->pm = pm; v->pm2 = pm2; v->amp = amp; v->idx = idx; v->idx2 = idx2;
}

/* shimmer: a modulator with self-feedback (the buzz, idx2) driving the carrier */
static void nv_render_shimmer(nvoice *v, float *out)
{
	uint32_t pc = v->pc, pm = v->pm, ic = v->ic, im = v->im;
	float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc, fbk = v->idx2, fb = v->fb;
	for (uint32_t i = 0; i < VSUB; i++) {
		float mo = SIN[(pm + (uint32_t)(int32_t)(fb * fbk)) >> TBL_SHIFT];
		fb = mo;
		out[i] += sin_u(pc + (uint32_t)(int32_t)(mo * idx)) * amp;
		pm += im;
		pc += ic;
		amp += ai;
		idx += ii;
	}
	v->pc = pc; v->pm = pm; v->amp = amp; v->idx = idx; v->fb = fb;
}

/* an idle voice among v[from .. from+n-1], else the quietest of them */
static nvoice *alloc_from(chan *c, int from, int n)
{
	nvoice *best = &c->v[from];
	for (int i = from; i < from + n; i++)
		if (c->v[i].stage == ST_IDLE)
			return &c->v[i];
	for (int i = from + 1; i < from + n; i++)
		if (c->v[i].amp < best->amp)
			best = &c->v[i];
	return best;
}

/* ================================================================ channel: voice logic */

static void chan_reset(chan *c, int voice)
{
	memset(c, 0, sizeof *c);
	c->voice = c->want = (uint8_t)voice;
	c->fade = 0.0f;
	c->last_note = -1;
	for (int i = 0; i < NV; i++) {
		c->v[i].side = (uint8_t)(i & 1);
		c->v[i].ratio = 1.0f;
		c->v[i].pc = rnd32();
		c->v[i].pm = rnd32();
		c->v[i].pc2 = rnd32();
		c->v[i].pm2 = rnd32();
		c->gap[i] = 0.3f + 2.5f * (float)i * rnd();   /* the pad comes in one breath at a time */
	}
	c->glide_note = c->target_note = -1.0f;
	for (int k = 0; k < 4; k++)
		for (int s = 0; s < BELL_MAX; s++)
			c->loop[k][s] = -1;
}

/* ---- bells: four tape loops ---- */
static void bells_fill(chan *c, int ch)
{
	float act = P.ch[ch][CP_ACTIVITY];
	for (int k = 0; k < 4; k++) {
		/* AMB-1's density: 0 to 5 notes a loop, swelled and thinned by the arc */
		int len = BELL_LEN[k], have = 0;
		int want = (int)clampf(act * 5.0f * (1.0f + 0.8f * arc) + rnd(), 0.0f, 5.0f);
		for (int s = 0; s < len; s++)
			have += c->loop[k][s] >= 0;
		while (have < want) {
			int s = rndi(len);
			if (c->loop[k][s] < 0) {
				c->loop[k][s] = (int8_t)rndi(scale_n + 3);
				have++;
			}
		}
		while (have > want) {
			int s = rndi(len);
			if (c->loop[k][s] >= 0) {
				c->loop[k][s] = -1;
				have--;
			}
		}
	}
}

static const float BELL_RATIO[3][4] = {
	{2.0f, 3.0f, 4.0f, 1.0f},      /* harmonic */
	{3.5f, 2.76f, 4.2f, 1.41f},    /* bell */
	{5.19f, 7.1f, 3.14f, 6.3f},    /* glass */
};

static void bells_beat(chan *c, int ch)
{
	int base = reg_base(c, ch) - 12;
	/* CHARACTER: its lower half the ratio set (harmonic, bell, glass) on the beat, its upper half
	 * the same three in sixteenths, busier */
	float chr = P.ch[ch][CP_CHAR];
	int zone = stepped(chr > 0.5f ? (chr - 0.5f) * 2.0f : chr * 2.0f, 3);
	for (int k = 0; k < 4; k++) {
		int len = BELL_LEN[k];
		int8_t d = c->loop[k][c->lpos[k]];
		if (d >= 0 && rnd() < 0.6f + 0.4f * eff_activity(ch)) {
			int oct = d / scale_n, deg = d % scale_n;
			int note = base + 12 * oct + ((scale_pc[deg] - root_pc + 12) % 12) + root_pc % 12;
			while (note < base)
				note += 12;
			/* a human hand: each bell up to 60 ms late, as AMB-1's (it also keeps four loops
			 * landing on one beat from all starting in the same millisecond) */
			c->bp_t[k] = 0.06f * rnd();
			c->bp_note[k] = (float)note;
			c->bp_vel[k] = 0.45f + 0.35f * rnd();
			c->bp_ratio[k] = BELL_RATIO[zone][k];
			c->bp_on |= (uint8_t)(1u << k);
			notes_fired++;
		}
		if (++c->lpos[k] >= len) {
			c->lpos[k] = 0;
			/* evolve: now and then a loop changes one note */
			if (rnd() < 0.15f + 0.6f * P.g[GP_MOTION]) {
				int s = rndi(len);
				if (c->loop[k][s] >= 0)
					c->loop[k][s] = (int8_t)rndi(scale_n + 3);
			}
			bells_fill(c, ch);
		}
	}
}

/* ---- pulse: three euclidean rings of 16, 12 and 7 sixteenths ----
 * ACTIVITY the hits, TONE the shaker's and click's brightness, LENGTH the decays, REGISTER the
 * kick's pitch (half an octave a step), CHARACTER the swing (the off sixteenths up to half a
 * step late). The click's ring is turned a little against the others. */
static int euclid_hit(int k, int n, int i) { return k > 0 && ((i * k) % n) < k; }
/* mask: 1 kick, 2 shaker, 4 click, 8 the bar's accent, 16 a shaker accent, 32 snare,
 * 64 an open hat (a longer shaker) */
static void pulse_fire(chan *c, int ch, int mask)
{
	notes_fired++;
	float t = eff_tone(ch), l = P.ch[ch][CP_LENGTH];
	float kp = fexp2((float)(stepped(P.ch[ch][CP_REGISTER], 5) - 2) * 0.5f);
	if (mask & 1) {
		c->pk_a = (mask & 8 ? 1.0f : 0.7f) * (0.85f + 0.15f * rnd());
		c->pk_f = 120.0f * kp;
		c->pk_fend = 48.0f * kp;
		c->pk_ph = 0.0f;
		c->pk_k = fexp2(-9.9658f / ((0.12f + 0.33f * l) * VSR));
		pump_tgt = 1.0f;                          /* Playworld's pump */
		c->glow = 1.0f;
	}
	if (mask & 2) {
		c->ps_a = (mask & 16 ? 0.6f : 0.35f) * (0.7f + 0.3f * rnd());
		c->ps_k = fexp2(-9.9658f / ((mask & 64 ? 0.12f + 0.2f * l : 0.02f + 0.07f * l) * GL_SR));
	}
	if (mask & 4) {
		c->pc_a = 0.5f + 0.2f * rnd();
		c->pc_ph = 0.0f;
		c->pc_inc = (1100.0f + 1400.0f * t) / VSR;
		c->pc_k = 0.992f;                         /* ~15 ms */
	}
	if (mask & 32) {                              /* the snare: a body near 190 Hz and a burst of noise */
		c->pc_a = 0.75f + 0.15f * rnd();
		c->pc_ph = 0.0f;
		c->pc_inc = (170.0f + 60.0f * t) / VSR;
		c->pc_k = fexp2(-9.9658f / (0.07f * VSR));
		c->sn_a = 0.8f + 0.2f * rnd();
		c->sn_k = fexp2(-9.9658f / ((0.08f + 0.15f * l) * GL_SR));
		c->glow = 1.0f;
	}
}
static void pulse_step(chan *c, int ch, uint32_t step)
{
	float h = eff_activity(ch), chr = P.ch[ch][CP_CHAR];
	int mask, s16 = (int)(step % 16u);
	float swing;
	if (chr <= 0.5f) {
		/* the lower half: three euclidean rings of 16, 12 and 7 against each other, swung up to
		 * half a step */
		int kk = 1 + (int)(h * 3.0f + 0.5f), hk = 2 + (int)(h * 6.0f + 0.5f), ck = (int)(h * 3.0f + 0.5f);
		int s12 = (int)(step % 12u), s7 = (int)((step + 3u) % 7u);
		mask = (euclid_hit(kk, 16, s16) ? 1 : 0) | (euclid_hit(hk, 12, s12) ? 2 : 0) |
		       (euclid_hit(ck, 7, s7) ? 4 : 0) | (s16 == 0 ? 8 : 0) | (s12 & 1 ? 0 : 16);
		swing = chr;
	} else {
		/* the upper half: a 4/4 grid with a backbeat. Kick on 1 and 3, then a boom-bap kick
		 * (1, the "and" of 2, the "and" of 3), then four on the floor; the snare on 2 and 4;
		 * the hats in eighths, then sixteenths, then with open hats on the offbeats. Swing grows
		 * across the half, to half a step. */
		static const uint16_t KICK[3] = {0x0101, 0x0441, 0x1111};   /* bit n: step n */
		int lvl = h < 0.4f ? 0 : h < 0.75f ? 1 : 2;
		mask = (KICK[lvl] >> s16) & 1;
		if (s16 == 4 || s16 == 12)
			mask |= 32;
		if (h < 0.4f ? !(s16 & 1) : 1)
			mask |= 2 | (!(s16 & 1) ? 16 : 0);
		if (h >= 0.85f && (s16 & 3) == 2)
			mask |= 64 | 16;
		if (s16 == 0)
			mask |= 8;
		swing = (chr - 0.5f);
	}
	if (form_fill) {
		/* Playworld's form: the last bar of a build is a fill, hats on every sixteenth and a
		 * snare roll through its second half, every step in its last beat */
		mask |= 2 | (s16 & 1 ? 0 : 16);
		if (s16 >= 8 && (s16 >= 12 || !(s16 & 1)))
			mask |= 32;
	}
	if (!(mask & (1 | 2 | 4 | 32)))
		return;
	if ((step & 1u) && swing > 0.01f) {      /* an off sixteenth: late */
		float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		c->pend = (uint8_t)mask;
		c->pend_t = swing * step_s;
		return;
	}
	pulse_fire(c, ch, mask);
}

/* semitones from the chord's root up d steps of the scale (d counted in a seven-note scale: 2 the
 * third, 4 the fifth, 6 the seventh, 7 the octave; a pentatonic scale's nearest step) */
static int deg_semis(int d)
{
	int n = scale_n > 0 ? scale_n : 7, k = n == 7 ? d : (d * n + 3) / 7;
	int at = chord_deg + k;
	int r0 = (scale_pc[chord_deg % n] - root_pc + 12) % 12, r1 = (scale_pc[at % n] - root_pc + 12) % 12;
	return r1 + 12 * (at / n) - r0 - 12 * (chord_deg / n);
}

/* how much a channel's macro pushes its rhythm off the beat (strings, seq, glow): its depth,
 * swelling and waning with its glide */
static float syncopation(int ch)
{
	float d = mac_depth[ch];
	return d <= 0.0f ? 0.0f : clampf(d * (0.55f + 0.45f * mac_u[ch][1]), 0.0f, 1.0f);
}

/* ---- plucked strings (kalimba, harp, harpsichord) ---- */
static int ks_count(int voice) { return voice == GV_KALIMBA ? 3 : KS_N; }
/* string k plucked at hz: its excitation (noise through a one-pole low-pass, ea), the loop's
 * filter a (0.5: dark, toward 1: bright), -60 dB in T seconds. The excitation is written into
 * the string during its first period by the render, so a pluck costs no spike. tuned: an
 * all-pass in the loop takes up the fraction of a sample the length can't (the harp's and the
 * harpsichord's top would otherwise sit up to a third of a semitone off) */
static void ks_pluck(chan *c, int k, float hz, float vel, float ea, float a, float T, int tuned)
{
	const int room = KS_BUF / ks_count(c->voice);
	float D = VSR / hz, na = 1.0f - a;
	int len = tuned ? (int)(D + na - 0.15f) : (int)D;
	len = len >= room ? room - 1 : len < 8 ? 8 : len;
	if (tuned) {
		float d = clampf(D - ((float)len - na), 0.15f, 1.15f);
		c->ks_c[k] = (1.0f - d) / (1.0f + d);
		c->ks_x1[k] = c->ks_y1[k] = 0.0f;
	}
	c->ks_ea[k] = ea;
	c->ks_elp[k] = 0.0f;
	c->ks_vel[k] = vel;
	c->ks_fill[k] = (uint16_t)len;
	c->ks_len[k] = (uint16_t)len;
	c->ks_i[k] = 0;
	c->ks_g[k] = fexp2(-9.9658f * (float)len / (VSR * T));
	c->ks_env[k] = vel;
	c->ks_kd[k] = fexp2(-9.9658f * (float)GL_SUB / (GL_SR * T));
	c->ks_a[k] = a;
	c->ks_hold[k] = 0.0f;
	notes_fired++;
}
/* the quietest of the channel's strings (an idle one first); -1 if none is free and the budget
 * is full */
static int ks_take(chan *c)
{
	int best = 0;
	for (int k = 0; k < KS_N; k++) {
		if (!c->ks_len[k])
			return budget_used() + 1 > fm_cap ? -1 : k;
		if (c->ks_env[k] < c->ks_env[best])
			best = k;
	}
	return best;
}
/* the harp: a string plucked, a finger's softness (TONE), ringing LENGTH (1.5..9 s), less up high */
static void harp_pluck(chan *c, int ch, int note, float vel)
{
	int k = alt_m[GV_HARP] == 3 ? 0 : ks_take(c);
	if (k < 0)
		return;
	float n = ivl_note(ch, (float)note), tone = eff_tone(ch);
	float T = expmap(P.ch[ch][CP_LENGTH] * (arp_live(ch) ? 0.12f : 1.0f), 1.5f, 9.0f) * clampf(1.0f - (n - 60.0f) * 0.025f, 0.35f, 1.3f);
	vel *= clampf(1.0f - (60.0f - n) * 0.035f, 0.45f, 1.0f);   /* low strings carry more: evened out */
	if (alt_m[GV_HARP] == 3) {                   /* audition: an FM harp (1:1, its index falling fast) */
		nvoice *v = alloc(c, 4);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, vel * 0.6f, 1.0f, c->vnext++);
		v->hold = T;
		c->glow = vel;
		return;
	}
	ks_pluck(c, k, mtof(n) * speed * pbend, vel * (alt_m[GV_HARP] == 2 ? 0.45f : 2.0f), 0.07f + 0.25f * tone, 0.5f + 0.2f * tone, T, 1);
	if (alt_m[GV_HARP] == 2)
		c->ks_hold[k] = -1.0f;                   /* audition: a finger's pull, not a burst of noise */
	else if (alt_m[GV_HARP] == 1 && mac_depth[ch] > 0.0f)   /* its macro moves the pluck toward a finger */
		c->ks_hold[k] = -0.999f * mac_depth[ch] * (0.5f + 0.5f * mac_u[ch][0]) - 0.001f;
	c->glow = vel;
}
/* the harpsichord: a quill's bright pluck; CHARACTER the stops: the lute (buff, dark and short),
 * the 8', the 8' and the 4' an octave above. The damper falls after the note's length (LENGTH) */
static void harpsi_pluck(chan *c, int ch, int note, float vel, float hold)
{
	int stop = stepped(P.ch[ch][CP_CHAR], 3);
	float tone = eff_tone(ch);
	for (int o = 0; o < (stop == 2 ? 2 : 1); o++) {
		int k = ks_take(c);
		if (k < 0)
			return;
		float n = ivl_note(ch, (float)note) + 12.0f * (float)o;
		float T = (stop == 0 ? 1.2f : 4.0f) * clampf(1.0f - (n - 60.0f) * 0.02f, 0.4f, 1.4f);
		float a = stop == 0 ? 0.52f : 0.66f + 0.2f * tone;
		ks_pluck(c, k, mtof(n) * speed * pbend, vel * (o ? 0.5f : 1.0f), stop == 0 ? 0.25f : 0.35f + 0.35f * tone, a, T, 1);
		c->ks_hold[k] = hold;
	}
	c->glow = vel;
}
/* funk: a new bar's line of sixteenths. 0 root, 1 octave pop, 2 fifth, 3 seventh, 4 ghost (a
 * muted thump), 5 a step up from below; +8 accented. On the one, always; the rest live on
 * the sixteenths between the beats, more so as its macro pushes */
static void funk_fill(chan *c, int ch)
{
	float act = eff_activity(ch), syn = 0.4f + 0.6f * syncopation(ch), slap = P.ch[ch][CP_CHAR];
	int8_t *L = c->loop[0];
	L[0] = 8;
	for (int s = 1; s < 16; s++) {
		float p = (s & 1) ? (0.12f + 0.45f * act) * (0.4f + syn) :
		          (s & 2) ? (0.15f + 0.45f * act) * (1.1f - 0.4f * syn) : (0.1f + 0.4f * act) * (1.0f - 0.4f * syn);
		L[s] = -1;
		if (rnd() >= p)
			continue;
		float r = rnd();
		int k = r < 0.4f ? 0 : r < 0.4f + 0.12f + 0.15f * slap * (s & 1) ? 1 : r < 0.68f ? 2 : r < 0.78f ? 3 : 4;
		if (s >= 14 && rnd() < 0.35f)
			k = 5;
		L[s] = (int8_t)(k + ((s & 3) == 2 && k != 4 && rnd() < 0.5f ? 8 : 0));
	}
}
/* ribbon: a note of the line (glided to when it is already sounding, the ribbon's swoop in
 * from below, now and then from above, when it isn't); it holds for LENGTH */
static void ribbon_note(chan *c, int ch, int base)
{
	nvoice *v = &c->v[0];
	int note = pick_note(c, base - 5, base + 9, 0.65f);
	float n = ivl_note(ch, (float)note), len = P.ch[ch][CP_LENGTH];
	int on = v->stage == ST_ATT || v->stage == ST_DEC;
	if (!on) {
		if (!sounding(v))
			make_room_w(v, 4);
		c->glide_note = n;                      /* a muted horn: it starts on the note */
		if (v->amp < 0.01f)
			v->t = 0.0f;
		v->stage = ST_ATT;
		v->vel = 0.7f + 0.25f * rnd();
	} else
		v->t = v->t < 0.25f ? v->t : 0.25f;    /* the hand presses again: a little less aftertouch */
	c->target_note = n;
	v->note = n;
	v->hold = expmap(len, 0.5f, 4.0f) * (0.6f + 0.8f * rnd());
	c->glow = 0.8f;
}

static const int8_t GLOW_OCT[4] = {0, -12, -24, -12};

/* ================================================================ audition methods */

/* n moved k steps along the scale */
static int scale_step(int n, int k)
{
	int dir = k > 0 ? 1 : -1;
	for (int s = 0; s != k; s += dir) {
		do
			n += dir;
		while (!in_scale(((n % 12) + 12) % 12));
	}
	return n;
}
/* the chord tone nearest n */
static int nearest_chord(int n)
{
	int best = n, bd = 99;
	for (int k = 0; k < chord_n; k++) {
		int m = place(chord_pc[k], (float)n, n - 6, n + 6), d = m > n ? m - n : n - m;
		if (d < bd) {
			bd = d;
			best = m;
		}
	}
	return best;
}
/* three voices of the chord, each moved as little as it can from where it was (common tones
 * held), kept apart and near home: a keyboard player's voice leading */
static void lead_voices(chan *c, int base, int *out, int differ)
{
	int ctr = base < 45 ? 45 : base > 62 ? 62 : base;
	int lo = ctr - 4, hi = ctr + 13;
	int sets[6][3], ns = 0, col[6];
	if (chord_n >= 4) {
		static const int8_t S[3][3] = {{0, 1, 2}, {1, 2, 3}, {0, 1, 3}};
		for (int i = 0; i < 3; i++, ns++) {
			for (int j = 0; j < 3; j++)
				sets[ns][j] = chord_pc[S[i][j]];
			col[ns] = 0;
		}
	} else {
		for (int j = 0; j < 3; j++)
			sets[0][j] = chord_pc[j % chord_n];
		col[0] = 0;
		ns = 1;
	}
	/* colours: the ninth for the root, the sixth for the fifth (a little dearer, so mostly used
	 * to keep a re-bowed chord from repeating) */
	int n9 = (chord_pc[0] + deg_semis(1) + 24) % 12, n6 = (chord_pc[0] + deg_semis(5) + 24) % 12;
	sets[ns][0] = n9; sets[ns][1] = chord_pc[1]; sets[ns][2] = chord_pc[2 % chord_n]; col[ns++] = 1;
	sets[ns][0] = chord_pc[0]; sets[ns][1] = chord_pc[1]; sets[ns][2] = n6; col[ns++] = 1;
	sets[ns][0] = n9; sets[ns][1] = chord_pc[1]; sets[ns][2] = n6; col[ns++] = 1;
	static const int8_t PERM[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
	int best = 1 << 30;
	for (int st = 0; st < ns; st++)
		for (int pm = 0; pm < 6; pm++) {
			int cand[3], cost = 0;
			for (int k = 0; k < 3; k++) {
				float from = c->v[k].note > 0.0f ? c->v[k].note : (float)(ctr + 4 * k);
				cand[k] = place(sets[st][PERM[pm][k]], from, lo, hi);
				int d = cand[k] - (int)(from + 0.5f);
				cost += d * d;
			}
			int mn = cand[0], mx = cand[0];
			for (int k = 1; k < 3; k++) {
				mn = cand[k] < mn ? cand[k] : mn;
				mx = cand[k] > mx ? cand[k] : mx;
			}
			if (cand[0] == cand[1] || cand[0] == cand[2] || cand[1] == cand[2])
				cost += 1000;
			if (mx - mn > 19)
				cost += 200;
			int dc = (mn + mx) / 2 - (ctr + 5);
			cost += dc * dc / 3 + col[st] * 14;
			/* never a voicing it had lately: its last four are remembered (in loop[2]); the same
			 * notes again cost too much, the older the less */
			if (differ || c->v[0].note > 0.0f)
				for (int h = 0; h < 4; h++) {
					int same = c->loop[2][3 * h] > 0;
					for (int k = 0; k < 3; k++) {
						int x = cand[k], hit = 0;
						for (int j = 0; j < 3; j++)
							hit |= x == c->loop[2][3 * h + j];
						same &= hit;
					}
					if (same)
						cost += 5000 - 1000 * h;
				}
			if (cost < best) {
				best = cost;
				for (int k = 0; k < 3; k++)
					out[k] = cand[k];
			}
		}
	for (int i = 11; i >= 3; i--)                  /* remembered */
		c->loop[2][i] = c->loop[2][i - 3];
	for (int k = 0; k < 3; k++)
		c->loop[2][k] = (int8_t)out[k];
}

/* ---- strings, as a string machine is played: whole chords held, each new chord voice-led
 * (common tones held on), ACTIVITY re-bowing it on the bar, the half bar or the beat. No
 * syncopation. Methods: 1 Solina (8' and 4' saws, a 12 dB low-pass, the three-line ensemble),
 * 2 Juno PWM (a pulse whose width sways, 24 dB, the Juno's chorus), 3 bowed (two saws a few
 * cents apart, the filter opening slower than the bow, a late vibrato, the voices spread) */
static void strings_step(chan *c, int ch, uint32_t step, float act, int base)
{
	/* even held, it moves on to another voicing every two bars: never one chord droning */
	int every = act < 0.35f ? 32 : act < 0.6f ? 16 : act < 0.82f ? 8 : 4;
	int fresh = c->seen_chord != chord_count || c->v[0].note <= 0.0f;
	if ((fresh && (step & 3u)) || !playing)
		return;                                   /* a new chord comes in on a beat */
	int rebow = !fresh && every && step % (uint32_t)every == 0u;
	if (!fresh && !rebow)
		return;
	c->seen_chord = chord_count;
	int nt[3] = {0, 0, 0};
	lead_voices(c, base, nt, rebow);                /* a re-bow moves to another voicing too */
	for (int k = 0; k < 3; k++) {
		nvoice *v = &c->v[k];
		float n = ivl_note(ch, (float)nt[k]);
		int moved = n != v->note;
		if (v->note <= 0.0f)
			v->ratio = n;                         /* its gliding pitch */
		v->note = n;
		float vel = 0.8f + 0.15f * rnd();
		if (moved || rebow || v->stage == ST_IDLE || v->stage == ST_REL) {
			v->hold = v->stage == ST_IDLE ? 0.0f : 0.035f;   /* a breath between bows */
			notes_fired++;
			v->stage = ST_ATT;
			v->vel = vel;
			v->t = 0.0f;
		}
	}
	c->glow = 0.8f;
}
static void strings_control(chan *c, int ch, float sdt, float tone, float len)
{
	const int m = alt_m[GV_STRINGS];
	const float chr = P.ch[ch][CP_CHAR];
	const float att = 0.12f + 0.55f * len, rel = 0.8f + 2.2f * len;
	static const float GAIN[4] = {0.0f, 0.6f, 0.5f, 1.0f};
	if (m == 2)
		c->lfo2 += sub_inc(0.37f + 0.3f * chr, speed);   /* the pulse width's sway */
	for (int k = 0; k < 3; k++) {
		nvoice *v = &c->v[k];
		if (!playing && v->stage != ST_IDLE)
			v->stage = ST_REL;
		if (v->stage == ST_ATT) {
			if (v->hold > 0.0f) {
				v->hold -= sdt;
				v->env *= dcoef(0.25f, sdt);
			} else {
				v->env += sdt / att;
				if (v->env >= 1.0f) {
					v->env = 1.0f;
					v->stage = ST_DEC;
				}
			}
		} else if (v->stage == ST_DEC) {
			v->env += (0.78f - v->env) * (1.0f - dcoef(6.0f, sdt));   /* settles a little */
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		v->t += sdt;
		if (v->note > 0.0f)
			v->ratio += (v->note - v->ratio) * (1.0f - dcoef(0.06f, sdt));   /* a quick legato */
		float hz = mtof(v->ratio > 1.0f ? v->ratio : 60.0f) * speed * pbend, e = v->env;
		if (m == 1) {
			v->ic = hz_inc(hz);
			v->ic2 = hz_inc(hz * 2.0f);
			float fc = (900.0f + 1800.0f * tone) * fexp2((v->ratio - 60.0f) * (1.0f / 48.0f));
			v->fk = clampf(fc * (6.2832f / VSR), 0.05f, 0.85f);
		} else if (m == 2) {
			v->ic = hz_inc(hz);
			v->bend = 0.5f - (0.1f + 0.16f * chr) * (0.5f + 0.5f * sin_u(c->lfo2 + (uint32_t)k * 0x55555555u));
			/* a filter envelope: open for the first 100 ms of a bow, then closing over about a
			 * second and a half to a third of the way, so a held chord darkens instead of droning */
			float fe = v->t < 0.1f ? 1.0f : 0.3f + 0.7f * fexp2(-(v->t - 0.1f) * 1.4f);
			float fc = (350.0f + 2600.0f * tone * tone * fe) * fexp2((v->ratio - 60.0f) * (1.0f / 24.0f));
			fc = clampf(fc, 1.5f * hz, 5000.0f);
			v->fk = clampf(1.0f - fexp2(-fc * (9.0647f / VSR)), 0.01f, 0.8f);
			v->fq = 0.25f;
		} else {
			float late = clampf(v->t - 0.5f, 0.0f, 1.0f) * (v->stage == ST_REL ? 0.0f : 1.0f);
			float vib = 1.0f + (0.002f + 0.002f * chr) * late * sin_u(lfo_vib + (uint32_t)k * 0x40000000u);
			v->ic = hz_inc(hz * vib * 1.0035f);
			v->ic2 = hz_inc(hz * vib / 1.0035f);
			float bow = clampf(v->t / (att * 1.6f), 0.0f, 1.0f);
			float fc = (300.0f + 2600.0f * tone * tone * (0.35f + 0.65f * bow * e)) * fexp2((v->ratio - 60.0f) * (1.0f / 24.0f));
			fc = clampf(fc, 1.3f * hz, 5000.0f);
			v->fk = clampf(1.0f - fexp2(-fc * (9.0647f / VSR)), 0.01f, 0.8f);
			v->fq = 0.2f;
		}
		v->idx = v->iinc = 0.0f;
		float a = e * v->vel * GAIN[m];
		v->ainc = (a - v->amp) * (1.0f / VSUB);
		if (v->stage == ST_IDLE && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}

/* ---- ribbon as a muted brass. Methods 1 and 2 play phrases (two to four notes on the beat,
 * stepwise, ending on a chord tone, rests between): 1 subtractive (two saws through a 24 dB
 * low-pass whose cutoff follows the loudness, in harmonics of the note), 2 FM brass (carrier and
 * modulator 1:1, the index rising with the envelope, kept low: muted). 3 is a brass section:
 * the chord in three FM-brass voices, voice-led, held, re-blown on the bar or half bar */
static void ribbon_blow(chan *c, int ch, int note, float vel, float gate)
{
	nvoice *v = &c->v[0];
	float n = ivl_note(ch, (float)note);
	if (alt_m[GV_RIBBON] == 2) {
		if (!sounding(v))
			make_room_w(v, 2);
		nv_trigger(v, (float)note, vel, 1.0f, 0);
	} else {
		if (!sounding(v))
			make_room_w(v, 4);
		v->note = n;
		v->vel = vel;
		v->stage = ST_ATT;
	}
	v->hold = gate;
	v->t = 0.0f;
	c->target_note = n;
	c->glide_note = n + 0.18f;                   /* blown hard: a touch sharp for an instant */
	c->glow = vel;
}
static void ribbon_step(chan *c, int ch, uint32_t step, float act, int base)
{
	const float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	if (alt_m[GV_RIBBON] == 3) {
		int every = act < 0.4f ? 0 : act < 0.75f ? 16 : 8;
		int fresh = c->seen_chord != chord_count || c->v[0].note <= 0.0f;
		if ((fresh && (step & 3u)) || !playing)
			return;
		int again = !fresh && every && step % (uint32_t)every == 0u;
		if (!fresh && !again)
			return;
		c->seen_chord = chord_count;
		int nt[3];
		if (fresh)
			lead_voices(c, base - 7, nt, 0);
		for (int k = 0; k < 3; k++) {
			nvoice *v = &c->v[k];
			if (!fresh)
				nt[k] = (int)(v->note + 0.5f);       /* (already moved by its interval) */
			if (!sounding(v))
				make_room_w(v, 2);
			float keep = v->note;
			nv_trigger(v, (float)nt[k], 0.75f + 0.15f * rnd(), 1.0f, k);
			if (!fresh)
				v->note = keep;
			v->hold = every ? (float)every * step_s * 0.9f : 64.0f * step_s;
		}
		c->glow = 0.8f;
		return;
	}
	if (c->arp_i) {
		c->arp_i--;
		return;
	}
	if (c->hq_i >= c->hq_n) {                   /* no phrase: perhaps one starts, on the bar */
		if ((step & 15u) || !playing || rnd() >= 0.3f + 0.5f * act)
			return;
		static const int8_t RH[8][5] = {{8, 8, 0}, {4, 4, 8, 0}, {12, 4, 0}, {16, 0}, {4, 4, 4, 4, 0},
		                                {8, 4, 4, 0}, {8, 8, 16, 0}, {4, 4, 8, 16, 0}};
		const int8_t *rh = RH[rndi(8)];
		int lo = base - 5, hi = base + 9, n = 0;
		int cur = place(chord_pc[rndi(chord_n)], (float)(c->last_note > 0 ? c->last_note : base + 2), lo, hi);
		while (n < 5 && rh[n]) {
			if (n) {
				int dir = cur > base + 5 ? -1 : cur < base - 2 ? 1 : (rnd() < 0.5f ? -1 : 1);
				cur = scale_step(cur, dir * (rnd() < 0.7f ? 1 : 2));
				if (n == 4 || !rh[n + 1])
					cur = nearest_chord(cur);        /* the phrase ends on a chord tone */
			}
			c->hq[n] = (int8_t)cur;
			c->loop[1][n] = rh[n];
			n++;
		}
		c->last_note = cur;
		c->hq_n = (uint8_t)n;
		c->hq_i = 0;
	}
	int d = c->loop[1][c->hq_i], last = c->hq_i + 1 == c->hq_n;
	ribbon_blow(c, ch, c->hq[c->hq_i], c->hq_i == 0 ? 0.85f : 0.72f, (float)d * step_s * (last ? 0.9f : 0.8f));
	c->hq_i++;
	c->arp_i = (uint8_t)(d - 1);
}
static void ribbon_control(chan *c, int ch, float sdt, float tone, float len)
{
	const int m = alt_m[GV_RIBBON];
	const float chr = P.ch[ch][CP_CHAR];
	const float att = 0.05f + 0.12f * len, rel = 0.25f + 0.8f * len;
	static const float GAIN[4] = {0.0f, 0.8f, 0.38f, 0.18f};
	c->glide_note += (c->target_note - c->glide_note) * (1.0f - dcoef(0.06f, sdt));
	for (int k = 0; k < (m == 3 ? 3 : 1); k++) {
		nvoice *v = &c->v[k];
		if (v->stage == ST_IDLE && v->amp == 0.0f) {
			v->ainc = v->iinc = 0.0f;
			continue;
		}
		if (!playing && v->stage != ST_IDLE)
			v->stage = ST_REL;
		if (v->stage == ST_ATT) {
			v->env += sdt / att;
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		}
		if (v->stage == ST_ATT || v->stage == ST_DEC) {
			v->hold -= sdt;
			if (v->stage == ST_DEC)                  /* the breath giving out: a long note falls away */
				v->env += ((m == 1 ? 0.2f : 0.85f) - v->env) * (1.0f - dcoef(m == 1 ? 1.0f + 2.5f * len : 1.5f, sdt));
			if (v->hold <= 0.0f)
				v->stage = ST_REL;
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		v->t += sdt;
		float e = v->env;
		float late = clampf((v->t - 0.9f) * (1.0f / 1.2f), 0.0f, 1.0f) * (v->stage == ST_REL ? 0.0f : 1.0f);
		float vib = 1.0f + 0.0015f * late * sin_u(lfo_vib + (uint32_t)k * 0x30000000u);
		float note = m == 3 ? v->note : c->glide_note;
		float hz = mtof(note) * speed * pbend * vib;
		if (m == 1) {
			v->ic = hz_inc(hz * 1.0012f);
			v->ic2 = hz_inc(hz / 1.0012f);
			float harm = 1.15f + (1.0f + 1.4f * tone + 1.6f * chr) * e;
			float fc = clampf(hz * harm * fexp2(-(note - 60.0f) * (1.0f / 60.0f)), 1.1f * hz, 3000.0f);
			v->fk = clampf(1.0f - fexp2(-fc * (9.0647f / VSR)), 0.005f, 0.8f);
			v->fq = 0.3f;
			v->idx = v->iinc = 0.0f;
			float a = e * v->vel * GAIN[1];
			v->ainc = (a - v->amp) * (1.0f / VSUB);
			if (v->stage == ST_IDLE && v->amp < 0.0002f)
				v->amp = v->ainc = 0.0f;
		} else {
			float idx = (0.25f + (0.7f + 1.0f * tone + 1.2f * chr) * e) * e;   /* index with the loudness */
			v->ic = hz_inc(hz);
			v->im = hz_inc(hz);
			ramp(v, e * v->vel * GAIN[m], idx * IDX_SCALE);
		}
	}
}

#include "glag_kit.h"

/* ---- the sixteenth-note step: every voice's sequencer ---- */
static void chan_step(chan *c, int ch, uint32_t step)
{
	float act = eff_activity(ch);
	int base = reg_base(c, ch);
	switch (c->voice) {
	case GV_BELLS:
		if (P.ch[ch][CP_CHAR] > 0.5f || (step & 3) == 0)
			bells_beat(c, ch);            /* quarter notes, as AMB-1's; or sixteenths */
		break;
	case GV_PULSE:
		pulse_step(c, ch, step);
		break;
	case GV_KIT:
		kit_step(c, ch, step);
		break;
	case GV_BASS: {
		int bar = (step & 15) == 0;
		int every = act < 0.25f ? 64 : act < 0.5f ? 32 : act < 0.75f ? 16 : 8;
		if ((bar && ((step % (uint32_t)every) == 0 || c->seen_chord != chord_count)) || c->target_note < 0.0f) {
			c->seen_chord = chord_count;
			int fifth = rnd() < 0.5f * P.ch[ch][CP_CHAR];
			int pc = fifth ? (chord_pc[0] + 7) % 12 : chord_pc[0];
			int note = place(pc, (float)base, base - 6, base + 6);
			c->target_note = ivl_note(ch, (float)note);
			if (c->glide_note < 0.0f)
				c->glide_note = (float)note;
			nv_trigger(&c->v[0], (float)note, 0.8f, 0.5f, 0);
			c->glow = 1.0f;
		}
		break;
	}
	case GV_GONG: {
		/* a strike on each new chord's root (on the bar), and now and then between */
		int strike = (step & 15) == 0 && c->seen_chord != chord_count;
		if (!strike && rnd() >= act * act * 0.035f)
			break;
		c->seen_chord = chord_count;
		int pc = strike || rnd() < 0.6f ? chord_pc[0] : chord_pc[1 + rndi(chord_n - 1)];
		int note = place(pc, (float)base, base - 5, base + 7);
		nvoice *v = alloc(c, 2);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, 0.75f + 0.2f * rnd(), 1.4f, 0);
		c->glow = 1.0f;
		break;
	}
	case GV_PIZZ: {
		int div = act < 0.25f ? 4 : act < 0.5f ? 3 : act < 0.75f ? 2 : 1;
		if (step % (uint32_t)div)
			break;
		/* up and down the chord over two octaves; CHARACTER adds octave jumps */
		int notes[8], n = 0;
		for (int o = 0; o < 2; o++)
			for (int k = 0; k < chord_n; k++)
				notes[n++] = place(chord_pc[k], (float)(base + 12 * o + 2 * k), base + 12 * o - 1, base + 12 * o + 11);
		int per = 2 * n - 2, q = c->arp_i % per, i = q < n ? q : per - q;
		int zone = stepped(P.ch[ch][CP_CHAR], 3), oct = 0;
		if (zone == 1)
			oct = (c->arp_i & 1) ? 12 : 0;
		else if (zone == 2) {
			static const int8_t JUMP[6] = {-12, 0, 0, 12, 12, 24};
			oct = JUMP[rndi(6)];
		}
		c->arp_i++;
		nvoice *v = alloc(c, 3);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)(notes[i] + oct), (((step & 3) == 0 ? 0.85f : 0.6f) + 0.15f * rnd()) * 1.5f, 1.0f, 0);
		c->glow = 0.7f;
		break;
	}
	case GV_PIANO:
	case GV_MARIMBA:
	case GV_FLUTE:
	case GV_KALIMBA:
	case GV_EP:
	case GV_SHIMMER:
	case GV_GLOW:
	case GV_TAILS: {
		static const float PMAX[GV_COUNT] = {0, 0, 0, 0.14f, 0.2f, 0, 0.28f, 0.07f, 0,
		                                     0, 0.14f, 0, 0, 0.07f, 0.08f, 0, 0, 0.12f, 0, 0, 0, 0, 0, 0, 0};
		float p = act * act * PMAX[c->voice];
		if (c->voice == GV_GLOW && (step & 15u) == 0u)
			c->arp_dir = (uint8_t)((c->arp_dir + 1u) & 3u);   /* full macro: a bar each at 0, -1, -2, -1 octaves */
		if (c->voice == GV_GLOW) {                    /* its macro leans it onto the off-beats */
			float syn = syncopation(ch);
			p *= (step & 1u) ? 1.0f + 1.5f * syn : (step & 2u) ? 1.0f : 1.0f - 0.6f * syn;
		}
		if (rnd() >= p)
			break;
		int note = pick_note(c, base - 5, base + 12, 0.6f);
		float vel = 0.55f + 0.45f * rnd();
		int vp = (int)(rnd32() & 1u);
		if (c->voice == GV_KALIMBA) {
			int k = c->ks_next;
			if (!c->ks_len[k] && budget_used() + 2 > fm_cap)
				break;                       /* no room: this pluck is skipped */
			c->ks_next = (uint8_t)((k + 1) % 3);
			float hz = mtof(ivl_note(ch, (float)note)) * speed * pbend;
			/* excitation: noise through a one-pole low-pass (TONE); -60 dB over LENGTH (0.4..6 s) */
			ks_pluck(c, k, hz, vel, 0.15f + 0.8f * eff_tone(ch), 0.5f + 0.45f * P.ch[ch][CP_CHAR],
			         expmap(P.ch[ch][CP_LENGTH], 0.4f, 6.0f), 0);
		} else if (c->voice == GV_SHIMMER) {
			/* a swoop up into the note, and its echo an octave higher a dotted eighth later */
			nvoice *v = alloc_from(c, 0, 2);
			if (!sounding(v))
				make_room(v);
			nv_trigger(v, (float)note, vel * 1.15f, 1.0f, 0);
			v->bend = -3.0f;
			c->echo_note = (float)(note + 12);
			c->echo_vel = vel * 0.55f;
			c->echo_at = 0.75f * 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		} else if (c->voice == GV_TAILS) {
			/* each tail its own ratio; now and then an octave up */
			static const float TR[6] = {1.0f, 2.0f, 1.5f, 3.5f, 0.5f, 2.5f};
			nvoice *v = alloc(c, 3);
			if (!sounding(v))
				make_room(v);
			nv_trigger(v, (float)(note + (rnd() < 0.2f ? 12 : 0)), vel * 0.9f, TR[rndi(6)], vp);
		} else if (c->voice == GV_EP) {
			nvoice *v = alloc(c, 3);
			if (!sounding(v))
				make_room_w(v, 3);
			nv_trigger(v, (float)note, vel, 1.0f, vp);
		} else {
			nvoice *v = alloc(c, 3);
			if (c->voice == GV_FLUTE && v->stage != ST_IDLE)
				break;                       /* a flute never cuts a breath short */
			float ratio = 1.0f;
			if (c->voice == GV_PIANO)
				ratio = (float)(1 + stepped(P.ch[ch][CP_CHAR], 3));
			else if (c->voice == GV_MARIMBA)
				ratio = 2.5f + 2.5f * P.ch[ch][CP_CHAR];
			if (c->voice == GV_GLOW && alt_m[GV_GLOW]) {
				note += GLOW_OCT[c->arp_dir & 3] * (mac_depth[ch] >= 0.99f);
				while (note > 79)
					note -= 12;                  /* a ceiling at G5 (784 Hz), leaps folded under it */
			}
			if (!sounding(v))
				make_room(v);
			nv_trigger(v, (float)note, c->voice == GV_FLUTE ? vel : vel * 1.35f, ratio, vp);
			if (c->voice == GV_FLUTE)
				v->hold = expmap(P.ch[ch][CP_LENGTH], 0.8f, 7.0f);
		}
		c->glow = vel;
		break;
	}
	case GV_STRINGS: {
		if (alt_m[GV_STRINGS]) {
			strings_step(c, ch, step, act, base);
			break;
		}
		/* a chord struck: on the bar, then (ACTIVITY) on the half bar, the beat, or eighths.
		 * Its macro adds syncopation: stabs on the off-beat (and the "and" after), some downbeats
		 * left out, as in dub chords. Each strike voiced afresh: an inversion, sometimes open
		 * (a tone an octave up), the seventh where there is one, now and then two notes; below C5 */
		/* syncopated by nature now (its macro pushes further), and a chord arp: each strike
		 * another voicing, never the last one again; often only two notes, sometimes rolled */
		int div = act < 0.3f ? 16 : act < 0.55f ? 8 : act < 0.8f ? 4 : 2;
		uint32_t ph = step % (uint32_t)div;
		float syn = 0.5f + 0.5f * syncopation(ch);
		int hit = ph == 0u;
		if (hit && rnd() < 0.35f * syn)
			hit = 0;
		if (!hit && div >= 4 &&
		    ((ph == (uint32_t)div / 2u && rnd() < 0.7f * syn) || (ph == (uint32_t)(3 * div) / 4u && rnd() < 0.3f * syn)))
			hit = 1;
		if (!hit)
			break;
		int top = base + 9 < 71 ? base + 9 : 71;
		int nn = rnd() < 0.45f ? 2 : 3, open = rnd() < 0.3f, roll = rnd() < 0.3f;
		int notes[3], sum = 0, inv = 0;
		for (int tries = 0; tries < 4; tries++) {
			/* walk the inversions (up, now and then back), a shape unlike the last */
			inv = (c->arp_i + 1 + (tries ? rndi(chord_n) : (rnd() < 0.25f ? chord_n - 2 : 0))) % chord_n;
			sum = 0;
			for (int k = 0; k < nn && k < chord_n; k++) {
				int idx = (k + inv) % chord_n;
				if (chord_n == 4 && k == nn - 1 && rnd() < 0.4f)
					idx = 3;                             /* the seventh for colour */
				int note = place(chord_pc[idx], (float)(base + 4 * k), base - 3, top);
				if (open && k == nn - 1 && note + 12 <= 71)
					note += 12;
				notes[k] = note;
				sum = sum * 131 + note;
			}
			if (sum != c->last_note)
				break;
		}
		c->arp_i = (uint8_t)inv;
		c->last_note = sum;
		float rdt = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]) * 0.25f;   /* a roll: a 64th apart */
		for (int k = 0; k < 3; k++) {
			nvoice *v = &c->v[k];
			if (k >= nn || k >= chord_n)
				continue;
			float n = ivl_note(ch, (float)notes[k]);
			v->vel = (ph == 0u ? 0.85f : 0.7f) + 0.2f * rnd();
			if (roll && k) {                             /* this one waits its turn */
				v->hold = n;
				c->cyc[k] = -rdt * (float)k;
				if (v->stage == ST_IDLE)
					v->stage = ST_REL;
				continue;
			}
			v->hold = 0.0f;
			v->note = n;
			v->menv = v->stage == ST_IDLE ? 0.0f : v->env;
			v->stage = ST_ATT;
			c->cyc[k] = 0.0f;
		}
		c->glow = 0.8f;
		break;
	}
	case GV_SEQ: {
		/* an analog arp. Its patterns (CHARACTER): octave bounce, broken thirds, a gallop, a
		 * wandering walk. Its macro syncopates: rests on the grid, notes pushed onto the
		 * sixteenth between */
		int div = act < 0.25f ? 8 : act < 0.5f ? 4 : act < 0.65f ? 3 : act < 0.8f ? 2 : 1;
		float syn = syncopation(ch);
		if (step % (uint32_t)div) {
			if (!(syn > 0.0f && div == 2 && rnd() < 0.3f * syn))
				break;
		} else if (syn > 0.0f && rnd() < 0.25f * syn)
			break;
		/* three octaves of the chord, from an octave below home */
		int notes[12], n = 0;
		for (int o = 0; o < 3; o++)
			for (int k = 0; k < chord_n; k++)
				notes[n++] = place(chord_pc[k], (float)(base - 12 + 12 * o + 2 * k), base - 13 + 12 * o, base - 1 + 12 * o);
		int pat = stepped(P.ch[ch][CP_CHAR], 4), q = c->arp_i++, note;
		if (pat == 0)                                /* each tone, then two octaves up */
			note = notes[(q >> 1) % chord_n] + ((q & 1) ? 24 : 0);
		else if (pat == 1)
			note = notes[((q >> 1) + (q & 1) * 2) % n];
		else if (pat == 2) {
			if ((q & 3) == 1)
				break;                           /* the gallop's rest: x . x x */
			note = notes[(q >> 2) % n];
		} else {
			int w = (int)c->arp_dir + (rnd() < 0.5f ? -1 : 1);
			w += rnd() < 0.25f ? (rnd() < 0.5f ? -2 : 2) : 0;   /* now and then a wider step */
			w = w < 0 ? 1 : w >= n ? n - 2 : w;
			c->arp_dir = (uint8_t)w;
			note = notes[w];
		}
		nvoice *v = alloc(c, 3);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, ((step & 15) == 0 ? 0.85f : 0.6f) + 0.15f * rnd(), 1.0f, 0);
		c->glow = 0.7f;
		break;
	}
	case GV_RIBBON: {
		if (alt_m[GV_RIBBON]) {
			ribbon_step(c, ch, step, act, base);
			break;
		}
		/* a CS-80 line: long notes, swooped into, held while the hand presses (aftertouch:
		 * brighter, more vibrato), sometimes glided straight on to the next (control) */
		nvoice *v = &c->v[0];
		if ((step & 1u) || v->stage == ST_ATT || v->stage == ST_DEC)
			break;
		float p = (0.04f + 0.3f * act * act) * ((step & 3u) == 0 ? 1.5f : 0.6f);
		if (rnd() < p)
			ribbon_note(c, ch, base);
		break;
	}
	case GV_FUNK: {
		/* a funk bassist: a bar of sixteenths written afresh on each new chord and now and then
		 * between, always on the one; ghost notes, octave pops, a step up into the root */
		uint32_t s = step & 15u;
		if (s == 0u && (c->seen_chord != chord_count || rnd() < 0.35f || c->loop[0][0] < 0)) {
			c->seen_chord = chord_count;
			funk_fill(c, ch);
		}
		int k = c->loop[0][s];
		if (k < 0)
			break;
		int acc = k >= 8;
		k &= 7;
		int root = place(chord_pc[0], (float)base, base - 5, base + 6), note = root;
		if (k == 1) note = root + 12;
		else if (k == 2) note = root + deg_semis(4);
		else if (k == 3) note = root + deg_semis(6);
		else if (k == 5) note = root - (in_scale((root + 11) % 12) ? 1 : 2);
		nvoice *v = &c->v[0];
		nv_trigger(v, (float)note, k == 4 ? 0.3f : (acc || s == 0u ? 0.95f : 0.75f) + 0.1f * rnd(), 1.0f, 0);
		v->hold = k == 4 ? 0.035f : expmap(P.ch[ch][CP_LENGTH], 0.07f, 0.6f) * (k == 1 ? 0.6f : 1.0f);
		v->bend = (k == 5 || (k == 0 && rnd() < 0.15f)) ? -1.0f : 0.0f;   /* slid into */
		v->ratio = k == 4 ? 0.0f : 1.0f;                                 /* a ghost: no quack */
		c->glow = k == 4 ? 0.3f : 0.9f;
		break;
	}
	case GV_HARP: {
		/* CHARACTER: single plucks, rolled chords, glissandi up (or down) the scale; ACTIVITY
		 * how often. While a roll or a glissando runs (control), nothing new starts */
		if (c->hq_i < c->hq_n)
			break;
		int kind = stepped(clampf(P.ch[ch][CP_CHAR] + 0.3f * (rnd() - 0.5f), 0.0f, 1.0f), 3);
		float p = kind == 0 ? ((step & 1u) ? 0.0f : 0.03f + 0.3f * act * act) :
		          kind == 1 ? ((step & 3u) ? 0.0f : 0.06f + 0.35f * act * act) :
		                      ((step & 15u) ? ((step & 1u) ? 0.0f : 0.02f + 0.15f * act * act) : 0.2f + 0.5f * act);
		if (rnd() >= p)
			break;
		if (kind == 0 || (kind == 2 && (step & 15u))) {
			harp_pluck(c, ch, pick_note(c, base - 5, base + 12, 0.6f), 0.55f + 0.35f * rnd());
			break;
		}
		int n = 0;
		if (kind == 1) {                             /* rolled: the chord from the bottom, 4 to 6 notes */
			int cnt = 4 + rndi(3);
			for (int i = 0; i < cnt; i++)
				c->hq[n++] = (int8_t)place(chord_pc[i % chord_n], (float)(base - 7 + 4 * i), base - 9 + 4 * i, base + 3 + 4 * i);
			c->hq_dt = 0.03f + 0.04f * rnd();
		} else {                                    /* a glissando: the scale over an octave and a half */
			int down = rnd() < 0.3f, from = base - 5 + rndi(5), cnt = scale_n + scale_n / 2 + 1;
			for (int m = from; n < cnt && n < 16 && m < from + 30; m++)
				if (in_scale(((m % 12) + 12) % 12))
					c->hq[n++] = (int8_t)m;
			if (down)
				for (int i = 0; i < n / 2; i++) {
					int8_t t = c->hq[i];
					c->hq[i] = c->hq[n - 1 - i];
					c->hq[n - 1 - i] = t;
				}
			c->hq_dt = 0.028f + 0.02f * rnd();
		}
		if (alt_m[GV_HARP]) {
			/* audition: each strum its own speed, slow, middling or fast; a roll easing out, a
			 * glissando slower at its ends than its middle; now and then a roll comes down */
			float r = rnd();
			float sp = r < 0.3f ? 2.6f + 1.2f * rnd() : r < 0.7f ? 1.0f + 0.4f * rnd() : 0.4f + 0.15f * rnd();
			c->hq_dt = (kind == 1 ? 0.04f : 0.035f) * sp;
			c->arp_dir = (uint8_t)(kind == 1 ? 0 : 1);
			if (kind == 1 && rnd() < 0.3f)
				for (int i = 0; i < n / 2; i++) {
					int8_t t = c->hq[i];
					c->hq[i] = c->hq[n - 1 - i];
					c->hq[n - 1 - i] = t;
				}
		}
		c->hq_n = (uint8_t)n;
		c->hq_i = 0;
		c->hq_t = 0.0f;
		c->hq_vel = 0.5f + 0.3f * rnd();
		break;
	}
	case GV_HARPSI: {
		/* Nyman's harpsichord: a figure running through the chord, written afresh on each new
		 * chord (Alberti, up and down, rising groups of three, a ground of root and octave), a bass
		 * note on the bar. ACTIVITY: quarters, eighths, sixteenths */
		int div = act < 0.3f ? 4 : act < 0.62f ? 2 : 1;
		if (c->seen_chord != chord_count && (step & 3u) == 0) {
			c->seen_chord = chord_count;
			c->arp_dir = (uint8_t)rndi(4);
			c->arp_i = 0;
		}
		float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		float hold = step_s * (float)div * (0.5f + 1.5f * P.ch[ch][CP_LENGTH]);
		if ((step & 15u) == 0u)
			harpsi_pluck(c, ch, place(chord_pc[0], (float)(base - 12), base - 18, base - 6), 0.8f, hold * 2.0f);
		if (step % (uint32_t)div)
			break;
		int t[8], n = 0;
		for (int o = 0; o < 2; o++)
			for (int k = 0; k < chord_n && k < 3; k++)
				t[n++] = place(chord_pc[k], (float)(base + 12 * o + 3 * k), base - 1 + 12 * o, base + 11 + 12 * o);
		int q = c->arp_i++, note;
		switch (c->arp_dir) {
		case 0: { static const int8_t AL[4] = {0, 2, 1, 2}; note = t[AL[q & 3]]; break; }
		case 1: { int per = 2 * n - 2, r = q % per; note = t[r < n ? r : per - r]; break; }
		case 2: note = t[((q / 3) + (q % 3)) % n]; break;
		default: note = (q & 1) ? t[n / 2 + (q >> 1) % (n - n / 2)] : t[0] + ((q & 2) ? 12 : 0); break;
		}
		harpsi_pluck(c, ch, note, ((q & 3) == 0 ? 0.8f : 0.6f) + 0.15f * rnd(), hold);
		break;
	}
	case GV_ARP: {
		int div = act < 0.25f ? 8 : act < 0.5f ? 4 : act < 0.65f ? 3 : act < 0.8f ? 2 : 1;
		if (step % (uint32_t)div)
			break;
		int pat = stepped(P.ch[ch][CP_CHAR], 7), note;
		float vel = 0.5f + 0.3f * rnd();
		if (pat >= 4) {
			/* a sequencer loop of 16, 7 or 5 steps on the chord's root, fifth, seventh and
			 * octave, as the Berlin school's: against the bar, the shorter ones turn over
			 * across it. The loop's first step is accented. */
			static const int8_t L16[16] = {0, 7, 4, 7, 6, 7, 4, 2, 0, 7, 4, 7, 9, 7, 4, 6};
			static const int8_t L7[7] = {0, 4, 7, 6, 4, 9, 7};
			static const int8_t L5[5] = {0, 4, 6, 7, 4};
			const int8_t *L = pat == 4 ? L16 : pat == 5 ? L7 : L5;
			int len = pat == 4 ? 16 : pat == 5 ? 7 : 5, q = c->arp_i % len;
			int root = place(chord_pc[0], (float)base, base - 6, base + 5);
			note = root + deg_semis(L[q]);
			vel = (q == 0 ? 0.8f : 0.5f) + 0.2f * rnd();
		} else {
			/* the chord over two octaves, walked by the CHARACTER pattern */
			int notes[8], n = 0;
			for (int o = 0; o < 2; o++)
				for (int k = 0; k < chord_n; k++)
					notes[n++] = place(chord_pc[k], (float)(base + 12 * o + 2 * k), base + 12 * o - 1, base + 12 * o + 11);
			int i;
			if (pat == 0)
				i = c->arp_i % n;
			else if (pat == 1) {
				int per = 2 * n - 2, q = c->arp_i % per;
				i = q < n ? q : per - q;
			} else if (pat == 2)
				i = rndi(n);
			else
				i = (c->arp_i & 1) ? (c->arp_i / 2) % n : 0;
			note = notes[i];
		}
		c->arp_i++;
		nvoice *v = alloc(c, 3);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, vel, 3.5f, c->arp_i);
		c->glow = 0.7f;
		break;
	}
	default:
		break;
	}
}

/* ---- per sub-block: envelopes, indices, pitches ---- */
static void chan_control(chan *c, int ch, float dt)
{
	float tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	if (arp_live(ch))
		len *= 0.12f;                          /* arp mode: the shortest decay and sustain */
	float sdt = dt * speed;                   /* time runs slower while slowing */
	int base = reg_base(c, ch);
	if (c->bp_on)                              /* bells waiting for their moment */
		for (int k = 0; k < 4; k++)
			if (c->bp_on & (1u << k)) {
				c->bp_t[k] -= sdt;
				if (c->bp_t[k] <= 0.0f) {
					c->bp_on &= (uint8_t)~(1u << k);
					if (!sounding(&c->v[k]))
						make_room(&c->v[k]);
					nv_trigger(&c->v[k], c->bp_note[k], c->bp_vel[k], c->bp_ratio[k], k);
					c->glow = 1.0f;
				}
			}
	if ((arp_live(ch) || midi_live(ch)) && arp_held(c->voice)) {   /* arp / MIDI: pads, strings... as notes */
		arp_held_control(c, ch, dt, tone, len);
		c->glow *= dcoef(0.4f, dt);
		return;
	}
	switch (c->voice) {
	case GV_PULSE:
		if (c->pend) {
			c->pend_t -= sdt;
			if (c->pend_t <= 0.0f) {
				pulse_fire(c, ch, c->pend);
				c->pend = 0;
			}
		}
		c->glow *= 0.95f;
		break;
	case GV_STRINGS: {
		if (alt_m[GV_STRINGS]) {
			strings_control(c, ch, sdt, tone, len);
			break;
		}
		/* Juno strings as chords, never held drones: each struck chord rises in 20 ms, falls in
		 * 50 ms to 60 %, holds a moment (LENGTH: 0.1 to 0.8 s), then rings away slowly
		 * (LENGTH: 1.5 to 5 s). One saw a side, the Juno's chorus swaying them. */
		const float chr = P.ch[ch][CP_CHAR];
		const float gate = 0.1f + 0.7f * len, rel = 1.5f + 3.5f * len;
		c->lfo1 += sub_inc(0.5f + 0.35f * chr, speed);
		float depth = (0.0012f + 0.0026f * chr) * sin_u(c->lfo1);
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			float e;
			if (v->hold > 0.0f && c->cyc[i] + sdt >= 0.0f) {   /* a rolled note's turn */
				v->note = v->hold;
				v->hold = 0.0f;
				v->menv = v->stage == ST_IDLE ? 0.0f : v->env;
				v->stage = ST_ATT;
				c->cyc[i] = -sdt;
			}
			if (v->stage == ST_IDLE) {
				e = 0.0f;
				if (v->hold > 0.0f)
					c->cyc[i] += sdt;                    /* still waiting its turn */
			} else {
				float t = (c->cyc[i] += sdt);
				if (t < 0.0f || v->stage == ST_REL)
					t = gate + 1.0f;                     /* waiting, or ringing away */
				if (t < 0.02f)                           /* from where it was: a re-strike never dips */
					e = v->menv + (1.0f - v->menv) * t / 0.02f;
				else if (t < 0.07f)
					e = 1.0f - 0.4f * (t - 0.02f) / 0.05f;
				else if (t < gate)
					e = 0.6f;
				else {
					v->env *= dcoef(rel, sdt);                 /* the long release */
					e = v->env;
					if (e < 0.0005f) {
						v->stage = ST_IDLE;
						e = 0.0f;
					}
				}
				if (t < gate)
					v->env = e;
			}
			float hz0 = mtof(v->note) * speed * pbend;
			(void)depth;                                /* the chorus does the moving now */
			v->ic = hz_inc(hz0);
			/* a 24 dB low-pass (the Juno's), opening a little on the strike: no fizz above it */
			float fc = (350.0f + 2600.0f * tone * tone * (0.55f + 0.45f * e)) * fexp2((v->note - 60.0f) * (1.0f / 24.0f));
			fc = clampf(fc, 1.5f * hz0, 5000.0f);
			v->fk = clampf(1.0f - fexp2(-fc * (9.0647f / VSR)), 0.01f, 0.8f);
			v->fq = 0.25f;
			v->idx = v->iinc = 0.0f;
			float a = e * v->vel * 0.9f;
			v->ainc = (a - v->amp) * (1.0f / VSUB);
			if (a <= 0.0f && v->amp < 0.0002f)
				v->amp = v->ainc = 0.0f;
		}
		break;
	}
	case GV_PAD:
	case GV_SUPER:
	case GV_SWELL: {
		/* the swell is a pad that breathes like brass: a slow rise, a hold, a long fall, its
		 * brightness following its loudness, vibrato arriving late */
		const int an = c->voice == GV_SUPER;                              /* super: swell-shaped */
		const int sw = c->voice == GV_SWELL || an;
		const float chr = P.ch[ch][CP_CHAR];
		int nv = 2 + stepped(P.ch[ch][CP_ACTIVITY], 2);   /* 2 or 3 voices breathing */
		float cycle = sw ? 6.0f + 24.0f * len : 4.0f + 26.0f * len;
		if (an)
			c->lfo1 += sub_inc(0.37f, speed);
		float ratio = sw ? 1.0f : (chr < 0.5f ? 1.0f : 2.0f);
		float detune = sw ? 0.0f : 0.12f * chr;
		float spread = fexp2((4.0f + 14.0f * chr) * (1.0f / 1200.0f));   /* swell's second carrier */
		for (int i = 0; i < NV; i++) {
			nvoice *v = &c->v[i];
			{	/* (frozen: the breaths under way finish; no new one starts) */
				if (v->stage == ST_IDLE) {
					c->gap[i] -= sdt;
					if (c->gap[i] <= 0.0f && playing && !frozen && i < nv &&
					    budget_used() + weight(c->voice) > fm_cap) {
						c->gap[i] = 0.5f;                /* no room: breathe a little later */
					} else if (c->gap[i] <= 0.0f && playing && !frozen && i < nv) {
						/* breathe in on this voice's tone of the chord */
						int pc = chord_pc[i % chord_n];
						float from = v->note > 0.0f ? v->note : (float)(base + 3 * i);
						v->note = ivl_note(ch, (float)place(pc, from, base - 2, base + 14));
						v->stage = ST_DEC;
						c->cyc[i] = 0.0f;
						v->vel = 0.5f + 0.2f * rnd();
						v->ratio = ratio;
						c->glow = 0.6f;
					}
				} else {
					c->cyc[i] += sdt / (cycle * PAD_FACTOR[i]);
					if (c->cyc[i] >= 1.0f) {
						v->stage = ST_IDLE;
						c->cyc[i] = 0.0f;
						c->gap[i] = rnd() * (1.2f - P.ch[ch][CP_ACTIVITY]) * cycle * 0.5f;
					}
				}
			}
			float x = c->cyc[i], e, idx, vib = 1.0f;
			if (v->stage == ST_IDLE)
				e = 0.0f;
			else if (!sw) {
				float s = sin_u((uint32_t)(x * 2147483648.0f));
				e = s * s;
			} else if (x < 0.45f) {
				float u = x * (1.0f / 0.45f);
				e = u * u;
			} else if (x < 0.7f)
				e = 1.0f;
			else
				e = 0.5f + 0.5f * sin_u((uint32_t)((0.25f + (x - 0.7f) * (0.5f / 0.3f)) * 4294967296.0f));
			v->env = e;
			if (sw) {
				idx = (0.15f + 2.2f * tone) * e * (0.4f + 0.6f * e);
				float vd = clampf((x - 0.3f) * 3.0f, 0.0f, 1.0f) * (0.002f + 0.005f * chr);
				vib = 1.0f + vd * sin_u(lfo_vib + (uint32_t)i * 0x40000000u);
			} else
				idx = tone * 1.3f * (0.5f + 0.5f * e);
			if (an) {
				/* strings: two saws a little apart, a slow chorus between them (Solina); super:
				 * five saws spread wider. CHARACTER is the spread; the filter opens with TONE and
				 * the swell */
				float hz0 = mtof(v->note) * speed * pbend * vib;
				int sup = c->voice == GV_SUPER;
				float d1, d2;
				if (sup) {
					float cents = 6.0f + 40.0f * chr;
					float ch1 = 1.0f + 0.25f * sin_u(c->lfo1 + (uint32_t)i * 0x55555555u);
					d1 = fexp2(cents * ch1 * (1.0f / 1200.0f));
					d2 = fexp2(cents * 2.1f * (1.0f / 1200.0f));
				} else {
					/* the Juno's chorus: one saw per side, their pitch swaying opposite ways
					 * (CHARACTER from chorus I, slow and gentle, toward II, faster and deeper) */
					float depth = (0.0016f + 0.0034f * chr) * sin_u(c->lfo1);
					d1 = 1.0f + depth;
					d2 = 1.0f;
				}
				v->ic = hz_inc(hz0 * d1);
				v->ic2 = hz_inc(hz0 / d1);
				(void)d2;
				if (sup) {
					v->im = hz_inc(hz0);
					v->ic3 = hz_inc(hz0 * d2);
					v->ic4 = hz_inc(hz0 / d2);
				}
				/* half keyboard tracking (a Juno's KYBD): high notes keep their brightness */
				/* a quarter keyboard tracking and a ceiling, so its top is bright, not shrill */
				float fc = (250.0f + 6500.0f * tone * tone * (0.35f + 0.65f * e)) * fexp2((v->note - 60.0f) * (1.0f / 48.0f));
				fc = fc > 5200.0f ? 5200.0f : fc;
				v->fk = clampf(fc * (6.2832f / VSR), 0.02f, 0.9f);
				v->idx = v->iinc = 0.0f;
				/* level with the pad, a little less up high; super 4 dB under it: it overbears a mix */
				float a = e * v->vel * (sup ? 0.366f : 0.58f) * clampf(1.0f - (v->note - 72.0f) * 0.025f, 0.6f, 1.0f);
				v->ainc = (a - v->amp) * (1.0f / VSUB);
				if (a <= 0.0f && v->amp < 0.0002f)
					v->amp = v->ainc = 0.0f;
				continue;
			}
			ramp(v, e * v->vel * (sw ? 0.7f : 1.0f), clampf(idx, 0.0f, 3.0f) * IDX_SCALE);
			float hz = mtof(v->note + detune * (float)(i - 1)) * speed * pbend * vib;
			v->ic = hz_inc(hz);
			v->im = hz_inc(hz * ratio);
			v->ic2 = hz_inc(hz * spread);
		}
		break;
	}
	case GV_BELLS:
		for (int k = 0; k < 4; k++) {
			nvoice *v = &c->v[k];
			/* as AMB-1's: a soft strike when dark (~50 ms, a slowed piano), sharp when bright
			 * (4 ms); low bells ring longer; the brightness fades on its own, longer when bright */
			float att = 0.004f + 0.046f * clampf(1.0f - 2.0f * tone, 0.0f, 1.0f);
			float ring = clampf(1.0f + (60.0f - v->note) * 0.015f, 0.5f, 1.6f);
			/* its macro, in the upper half of its glide, mutes the bell: a hand on it, a felt
			 * mallet: shorter, darker, a softer strike */
			float mu = mac_depth[ch] * clampf(mac_u[ch][1], 0.0f, 1.0f) *
			           clampf(0.15f + (v->note - 66.0f) * (1.0f / 18.0f), 0.15f, 1.0f);   /* mostly the high bells */
			nv_perc(v, att + 0.02f * mu, expmap(len, 1.0f, 10.0f) * ring * (1.0f - 0.8f * mu) / speed,
			        (0.35f + 1.5f * tone) * (1.0f - 0.6f * mu) / speed, (0.3f + 2.0f * tone) * (1.0f - 0.65f * mu), dt);
			nv_pitch(v, v->note);
		}
		break;
	case GV_GLOW:
	case GV_SEQ: {
		/* glow: a soft sine-and-triangle tone (CHARACTER: how much triangle), through a gentle
		 * low-pass (TONE), a slow swell in, LENGTH long; seq: a snappy saw, its resonant filter
		 * opening on each note and closing over LENGTH, TONE the cutoff */
		const int sq = c->voice == GV_SEQ;
		float dec = sq ? expmap(len, 0.12f, 1.6f) : expmap(len, 0.8f, 8.0f);
		float mdec = sq ? expmap(len, 0.04f, 0.7f) : 1.0f;
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			nv_perc(v, sq ? 0.002f : alt_m[GV_GLOW] ? 0.07f : 0.03f, dec / speed, mdec / speed, 1.0f, dt);
			float vib = sq ? 1.0f : 1.0f + 0.0025f * sin_u(lfo_vib + (uint32_t)i * 0x30000000u);
			v->ic = hz_inc(mtof(v->note) * speed * pbend * vib);
			v->idx = v->iinc = 0.0f;
			if (sq) {
				/* a polysynth's arp rather than a 303: the filter mostly open, a gentle snap,
				 * little resonance */
				float fc = (600.0f + 3400.0f * tone * tone + 1400.0f * v->menv * tone) * fexp2((v->note - 55.0f) * (1.0f / 24.0f));
				v->fk = clampf(2.0f * sin_u((uint32_t)(clampf(fc / VSR, 0.0f, 0.16f) * 2147483648.0f)), 0.01f, 0.9f);
				v->fq = 1.1f;                          /* resonance: a touch */
			} else {
				/* mostly triangle (CHARACTER: from half to all), a darker filter that closes a little
				 * as notes climb past C5, and a little quieter up there: soft, never shrill */
				if (alt_m[GV_GLOW]) {
					/* audition. 1 clean: the triangle's third and fifth, gone by 3 kHz; 2 warm: the
					 * second and third (an ocarina's roundness); 3 dark: a triangle through a fixed
					 * low-pass near 1 kHz that high notes fall into. All quieter as they climb */
					const int gm = alt_m[GV_GLOW];
					float f0 = mtof(v->note), chr = P.ch[ch][CP_CHAR];
					float mm = (0.5f + 0.5f * chr) * (0.7f + 0.6f * tone), w2 = 0.0f, w3, w5 = 0.0f, g;
					float up = clampf(v->note - 64.0f, 0.0f, 36.0f);
					if (gm == 1) {
						w3 = mm * (1.3f / 9.0f) * clampf((3000.0f - 3.0f * f0) * (1.0f / 1500.0f), 0.0f, 1.0f);
						w5 = mm * (1.3f / 25.0f) * clampf((3000.0f - 5.0f * f0) * (1.0f / 1500.0f), 0.0f, 1.0f);
						g = fexp2(-up * (0.5f / 6.0206f));
					} else if (gm == 2) {
						/* warm: the octave above (an even harmonic: round, not piercing) kept up to
						 * 4 kHz, a little third. Its macro, while its glide is in the upper half, blends
						 * up to 30 % of the breathy one in: the breath, and less of the octave */
						float b = 0.3f * mac_depth[ch] * clampf(mac_u[ch][1], 0.0f, 1.0f);
						/* and from C3 up its overtones fade away, gone by G3: up there it is the note
						 * alone, nothing to be shrill with */
						/* from C3 up, nothing above 800 Hz: each overtone fades out between 600 and 800 */
						const int cut = v->note >= 48.0f;
						const float k2 = cut ? clampf((800.0f - 2.0f * f0) * (1.0f / 200.0f), 0.0f, 1.0f) : 1.0f;
						const float k3 = cut ? clampf((800.0f - 3.0f * f0) * (1.0f / 200.0f), 0.0f, 1.0f) : 1.0f;
						w2 = k2 * (1.0f - b) * mm * 0.3f * clampf((4000.0f - 2.0f * f0) * (1.0f / 1500.0f), 0.0f, 1.0f);
						w3 = k3 * ((1.0f - b) * mm * 0.06f * clampf((3000.0f - 3.0f * f0) * (1.0f / 1500.0f), 0.0f, 1.0f) +
						     b * mm * (0.6f / 9.0f) * clampf((2500.0f - 3.0f * f0) * (1.0f / 1200.0f), 0.0f, 1.0f));
						v->bend = b * (0.02f + 0.04f * chr + 0.08f * v->menv) * (0.6f + 0.6f * tone);
						v->ratio = clampf(2.0f * sin_u((uint32_t)(clampf(f0 / VSR, 0.0f, 0.2f) * 2147483648.0f)), 0.01f, 1.2f);
						g = fexp2(-clampf(v->note - 52.0f, 0.0f, 36.0f) * (0.5f / 6.0206f));
					} else {
						/* breathy: a sine with breath around it (noise through a narrow band at the
						 * note, more on the attack), as a flute or ocarina stays sweet up high */
						w3 = mm * (0.6f / 9.0f) * clampf((2500.0f - 3.0f * f0) * (1.0f / 1200.0f), 0.0f, 1.0f);
						v->bend = (0.02f + 0.04f * chr + 0.08f * v->menv) * (0.6f + 0.6f * tone);   /* the breath's level */
						v->ratio = clampf(2.0f * sin_u((uint32_t)(clampf(f0 / VSR, 0.0f, 0.2f) * 2147483648.0f)), 0.01f, 1.2f);
						g = fexp2(-up * (0.5f / 6.0206f));
					}
					v->idx2 = w2;
					v->fq = w3;
					v->fk = w5;
					v->hold = g;
					continue;
				}
				/* a triangle's third and fifth (1/9, 1/25; CHARACTER and TONE from half to a little
				 * past them), each faded out as it climbs past 2.5 kHz and gone by 4 kHz, where the
				 * ear is keenest. And quieter as it climbs past E4, about 0.4 dB a semitone (the
				 * equal-loudness curves): a high note no longer stands out of the mix */
				float f0 = mtof(v->note), chr = P.ch[ch][CP_CHAR];
				float m = (0.5f + 0.5f * chr) * (0.7f + 0.6f * tone);
				float h3 = clampf((4000.0f - 3.0f * f0) * (1.0f / 1500.0f), 0.0f, 1.0f);
				float h5 = clampf((4000.0f - 5.0f * f0) * (1.0f / 1500.0f), 0.0f, 1.0f);
				v->fq = m * h3 * (1.0f / 9.0f) * 1.3f;
				v->fk = m * h5 * (1.0f / 25.0f) * 1.3f;
				v->hold = fexp2(-clampf(v->note - 64.0f, 0.0f, 36.0f) * (0.4f / 6.0206f));
			}
		}
		break;
	}
	case GV_RIBBON: {
		if (alt_m[GV_RIBBON]) {
			ribbon_control(c, ch, sdt, tone, len);
			break;
		}
		/* a muted brass (after the CS-80's soft brass): two saws a few cents apart through a
		 * 24 dB low-pass that sits low, about two to three harmonics up, and opens only a little
		 * as each note blows in, settling back as it holds (the brass "blat", kept gentle).
		 * CHARACTER lifts the mute: from nearly closed toward a brighter horn. Legato notes slide
		 * in a few hundredths of a second; a faint vibrato arrives late in long notes */
		nvoice *v = &c->v[0];
		const float chr = P.ch[ch][CP_CHAR];
		if (c->target_note < 0.0f) {
			v->ainc = 0.0f;
			break;
		}
		c->glide_note += (c->target_note - c->glide_note) * (1.0f - dcoef(0.12f, sdt));
		if (v->stage == ST_ATT) {
			v->env += sdt / (0.07f + 0.15f * len);
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		} else if (v->stage == ST_DEC) {
			v->hold -= sdt;
			if (v->hold <= 0.0f) {
				if (playing && rnd() < 0.25f + 0.3f * eff_activity(ch))
					ribbon_note(c, ch, base);          /* on to the next, legato */
				else
					v->stage = ST_REL;
			}
		} else if (v->stage == ST_REL || (!playing && v->stage != ST_IDLE)) {
			v->stage = ST_REL;
			v->env *= dcoef(0.35f + 1.2f * len, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		v->t += sdt;
		/* the filter's own envelope: up quickly with the breath, back to 60 % over 0.4 s */
		float fe = v->stage == ST_REL ? v->env : v->t < 0.08f ? v->t * (1.0f / 0.08f) :
		           0.6f + 0.4f * fexp2(-(v->t - 0.08f) * 3.6f);
		float late = clampf((v->t - 0.8f) * (1.0f / 1.5f), 0.0f, 1.0f);
		c->lfo1 += sub_inc(4.8f, speed);
		float vib = 1.0f + 0.0018f * late * (v->stage == ST_REL ? 0.0f : 1.0f) * sin_u(c->lfo1);
		float hz = mtof(c->glide_note) * speed * pbend * vib;
		v->ic = hz_inc(hz * 1.0017f);
		v->ic2 = hz_inc(hz * (1.0f / 1.0017f));
		/* cutoff in harmonics of the note (tracking a little under 1:1), capped low */
		float harm = (1.3f + 1.2f * chr) + (0.8f + 1.6f * chr + 1.2f * tone) * fe;
		float fc = hz * harm * fexp2(-(c->glide_note - 60.0f) * (1.0f / 72.0f));
		float cap = 1400.0f + 1800.0f * chr;
		cap = cap < 1.6f * hz ? 1.6f * hz : cap;           /* never below the note itself */
		fc = clampf(fc, 1.2f * hz, cap);
		v->fk = clampf(1.0f - fexp2(-fc * (9.0647f / VSR)), 0.005f, 0.8f);   /* 1 - e^(-2 pi fc / sr) */
		v->fq = 0.35f;                             /* a little resonance: a horn's body */
		float a = v->env * v->vel * 0.8f;
		v->ainc = (a - v->amp) * (1.0f / VSUB);
		if (v->stage == ST_IDLE && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
		break;
	}
	case GV_FUNK: {
		/* fingers to slap (CHARACTER): the envelope filter's quack, its resonance and sweep grow;
		 * TONE the filter's floor; each note held its length, then let go quickly */
		nvoice *v = &c->v[0];
		const float slap = P.ch[ch][CP_CHAR];
		if (v->stage == ST_IDLE && v->amp == 0.0f) {
			v->ainc = 0.0f;
			break;
		}
		if (v->stage == ST_ATT) {
			v->env += sdt / 0.003f;
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		} else if (v->stage == ST_DEC) {
			v->env += (0.7f - v->env) * (1.0f - dcoef(0.4f, sdt));
			v->hold -= sdt;
			if (v->hold <= 0.0f)
				v->stage = ST_REL;
		} else {
			v->env *= dcoef(0.12f, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		v->menv *= dcoef(0.12f + 0.35f * (1.0f - slap), sdt);
		v->bend *= dcoef(0.12f, sdt);
		float hz = mtof(v->note + v->bend) * speed * pbend;
		v->ic = hz_inc(hz);
		float q = v->menv * v->vel * v->ratio;
		float fc = (120.0f + 700.0f * tone * tone + q * (250.0f + 3200.0f * slap)) * fexp2((v->note - 40.0f) * (1.0f / 24.0f));
		fc = clampf(fc, 1.2f * hz, 5000.0f);
		v->fk = 2.0f * sin_u((uint32_t)(clampf(fc / VSR, 0.0f, 0.16f) * 2147483648.0f));
		v->fq = 1.3f - 0.85f * slap;
		v->idx = v->iinc = 0.0f;
		float a = v->env * v->vel * 0.9f;
		v->ainc = (a - v->amp) * (1.0f / VSUB);
		if (v->stage == ST_IDLE && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
		break;
	}
	case GV_HARP:
		/* a roll or glissando under way: its next notes as their time comes */
		if (c->hq_i < c->hq_n) {
			c->hq_t -= sdt;
			while (c->hq_t <= 0.0f && c->hq_i < c->hq_n) {
				int last = c->hq_i + 1 == c->hq_n;
				harp_pluck(c, ch, c->hq[c->hq_i++], c->hq_vel * (last ? 1.0f : 0.75f + 0.2f * rnd()));
				float f = 1.0f;
				if (alt_m[GV_HARP]) {
					float x = c->hq_n > 1 ? (float)c->hq_i / (float)(c->hq_n - 1) : 0.0f;
					f = c->arp_dir ? 1.0f + 0.55f * sin_u((uint32_t)(x * 4294967296.0f) + 0x40000000u) : 1.0f + 0.25f * x;
				}
				c->hq_t += c->hq_dt * f;
			}
		}
		if (alt_m[GV_HARP] == 3)                     /* FM harp: the plucks as note voices */
			for (int i = 0; i < 4; i++) {
				nvoice *v = &c->v[i];
				if (v->stage == ST_IDLE && v->amp == 0.0f) {
					v->ainc = v->iinc = 0.0f;
					continue;
				}
				nv_perc(v, 0.002f, v->hold / speed, 0.16f / speed, 0.5f + 2.0f * tone, dt);
				nv_pitch(v, v->note);
			}
		break;
	case GV_HARPSI:
		/* the dampers falling on notes whose time is up */
		for (int k = 0; k < KS_N; k++)
			if (c->ks_len[k] && c->ks_hold[k] > 0.0f) {
				c->ks_hold[k] -= sdt;
				if (c->ks_hold[k] <= 0.0f) {
					c->ks_hold[k] = 0.0f;
					c->ks_g[k] = fexp2(-9.9658f * (float)c->ks_len[k] / (VSR * 0.15f));
					c->ks_kd[k] = fexp2(-9.9658f * (float)GL_SUB / (GL_SR * 0.15f));
				}
			}
		break;
	case GV_PIANO:
	case GV_MARIMBA:
	case GV_ARP: {
		float dec, mdec, imax;
		if (c->voice == GV_PIANO) {
			dec = expmap(len, 0.8f, 8.0f);
			mdec = 0.35f;
			imax = 0.4f + 2.4f * tone;
		} else if (c->voice == GV_MARIMBA) {
			dec = expmap(len, 0.2f, 2.5f);
			mdec = 0.08f;
			imax = 0.3f + 2.2f * tone;
		} else {
			dec = expmap(len, 0.3f, 4.0f);
			mdec = 0.15f;
			imax = 0.2f + 1.8f * tone;
		}
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			nv_perc(v, c->voice == GV_PIANO ? 0.002f : 0.001f, dec / speed, mdec / speed, imax, dt);
			nv_pitch(v, v->note);
		}
		break;
	}
	case GV_FLUTE:
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			float att = 0.4f + 1.2f * len;
			if (v->stage == ST_ATT) {
				v->env += sdt / att;
				if (v->env >= 1.0f) {
					v->env = 1.0f;
					v->stage = ST_DEC;
				}
			} else if (v->stage == ST_DEC) {
				v->hold -= sdt;
				if (v->hold <= 0.0f)
					v->stage = ST_REL;
			} else if (v->stage == ST_REL) {
				v->env *= dcoef(0.6f + 2.0f * len, sdt);
				if (v->env < 0.0005f) {
					v->env = 0.0f;
					v->stage = ST_IDLE;
				}
			}
			/* breath: the attack is breathier (less feedback) and brightens as it opens */
			float a = v->env * v->vel;
			float fbk = (0.1f + 1.1f * tone) * (0.4f + 0.6f * v->env);
			/* self-feedback is as rich as a saw: high notes get less, or it folds back */
			float top = (IDX_TOP / (mtof(v->note) * speed * pbend) - 1.0f) * 0.25f;
			fbk = fbk < top ? fbk : (top > 0.0f ? top : 0.0f);
			ramp(v, a, fbk * IDX_SCALE);
			float vib = 1.0f + (0.0015f + 0.006f * P.ch[ch][CP_CHAR]) * sin_u(lfo_vib + (uint32_t)i * 1000000000u);
			v->ic = hz_inc(mtof(v->note) * speed * pbend * vib);
		}
		break;
	case GV_BASS: {
		nvoice *v = &c->v[0];
		if (c->target_note >= 0.0f) {
			float g = 0.02f + 1.5f * P.ch[ch][CP_CHAR];          /* glide time */
			c->glide_note += (c->target_note - c->glide_note) * (1.0f - dcoef(g * 0.3f, sdt));
		}
		float dec = expmap(len, 1.5f, 30.0f);
		if (v->stage == ST_ATT) {
			v->env += sdt / 0.03f;
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		} else if (v->stage == ST_DEC) {
			v->env *= dcoef(playing && !frozen ? dec : 2.0f, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		float a = v->env * v->vel;
		/* TONE: the sub-octave fades out and a little self-modulation (grit) comes in */
		ramp(v, a, (0.05f + 0.6f * tone) * IDX_SCALE);
		v->ratio = 1.0f - tone;                                /* sub mix */
		float hz = mtof(c->glide_note < 0.0f ? (float)base : c->glide_note) * speed * pbend;
		v->ic = hz_inc(hz);
		v->im = hz_inc(hz * 0.5f);
		break;
	}
	case GV_DRONE: {
		/* root and its interval, gliding to each new chord */
		static const float IVL[4] = {7.0f, 5.0f, 12.0f, 14.0f};
		if (c->seen_chord != chord_count || c->last_note != base) {
			c->seen_chord = chord_count;
			c->last_note = base;
			c->target_note = ivl_note(ch, (float)place(chord_pc[0], c->glide_note < 0.0f ? (float)base : c->glide_note, base - 5, base + 6));
		}
		if (c->glide_note < 0.0f)
			c->glide_note = c->target_note;
		c->glide_note += (c->target_note - c->glide_note) * (1.0f - dcoef(3.0f, sdt));
		float want = playing ? 1.0f : 0.0f;
		if (!frozen)
			c->swell += (want - c->swell) * (1.0f - dcoef(6.0f, sdt));
		float period = expmap(1.0f - P.ch[ch][CP_ACTIVITY], 12.0f, 80.0f);
		c->lfo1 += sub_inc(1.0f / period, speed);
		c->lfo2 += sub_inc(0.61f / period, speed);
		float breath = 1.0f - len * 0.6f * (0.5f - 0.5f * sin_u(c->lfo2));
		for (int k = 0; k < 2; k++) {
			nvoice *v = &c->v[k];
			float lfo = 0.5f - 0.5f * sin_u(c->lfo1 + (k ? 0x5EB851EBu : 0u));
			float idx = (0.2f + 2.3f * tone * lfo) * (k ? 0.75f : 1.0f);
			float a = c->swell * breath * (k ? 0.3f : 0.45f);
			ramp(v, a, clampf(idx, 0.0f, 3.0f) * IDX_SCALE);
			v->ratio = 2.0f + (k ? 0.0f : 0.003f);
			v->side = (uint8_t)k;
			nv_pitch(v, c->glide_note - 12.0f + (k ? IVL[stepped(P.ch[ch][CP_CHAR], 4)] : 0.0f));
			v->stage = c->swell > 0.0005f ? ST_DEC : ST_IDLE;
		}
		break;
	}
	case GV_EP:
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = v->iinc2 = 0.0f;
				continue;
			}
			/* TONE: dull to bright, and harder notes are brighter, as on the DX7 */
			nv_perc(v, 0.002f, expmap(len, 1.0f, 10.0f) / speed, 0.6f / speed,
			        (0.3f + 2.2f * tone) * (0.5f + 0.6f * v->vel), dt);
			v->menv2 *= dcoef(0.09f / speed, dt);
			float i2 = idx_cap(v, P.ch[ch][CP_CHAR] * 2.6f * v->menv2 * v->vel * IDX_SCALE, 14.0f / (v->ratio > 0.1f ? v->ratio : 1.0f));
			v->iinc2 = (i2 - v->idx2) * (1.0f / VSUB);
			nv_pitch(v, v->note);
			v->im2 = hz_inc(mtof(v->note) * speed * pbend * 14.0f);
		}
		break;
	case GV_GONG: {
		static const float GR[4] = {1.4f, 1.73f, 2.76f, 3.14f};
		float ratio = GR[stepped(P.ch[ch][CP_CHAR], 4)];
		for (int k = 0; k < 2; k++) {
			nvoice *v = &c->v[k];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			v->t += sdt;
			nv_perc(v, 0.003f, expmap(len, 4.0f, 25.0f) / speed, 1.0f, 1.0f, dt);
			/* the index blooms for 0.6 s after the strike, then fades over seconds */
			float b = v->t < 0.6f ? v->t * (1.0f / 0.6f)
			                      : fexp2(-clampf(v->t - 0.6f, 0.0f, 60.0f) * (1.4427f / 5.0f));
			float im = idx_cap(v, (0.4f + 2.4f * tone) * (0.25f + 0.75f * b) * IDX_SCALE, 1.0f);
			v->iinc = (im - v->idx) * (1.0f / VSUB);
			v->ratio = ratio;
			nv_pitch(v, v->note + 0.7f * fexp2(-clampf(v->t, 0.0f, 5.0f) * (1.4427f / 0.12f)));
		}
		break;
	}
	case GV_PIZZ:
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			nv_perc(v, 0.0005f, expmap(len, 0.06f, 0.45f) / speed, 0.03f / speed, 0.6f + 2.0f * tone, dt);
			nv_pitch(v, v->note);
		}
		break;
	case GV_SHIMMER: {
		if (c->echo_at > 0.0f) {
			c->echo_at -= sdt;
			if (c->echo_at <= 0.0f && playing) {
				nvoice *v = alloc_from(c, 2, 2);
				if (!sounding(v))
					make_room(v);
				nv_trigger(v, c->echo_note, c->echo_vel, 1.0f, 1);
				v->bend = -2.0f;
				c->glow = 0.5f;
			}
		}
		float trem = 1.0f - 0.22f * (0.5f + 0.5f * sin_u(lfo_trem));
		float fbk = (0.4f + 1.4f * P.ch[ch][CP_CHAR]) * IDX_SCALE;
		for (int k = 0; k < 4; k++) {
			nvoice *v = &c->v[k];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			v->t += sdt;
			nv_perc(v, 0.04f + 0.2f * len, expmap(len, 1.5f, 8.0f) / speed, 3.0f / speed, 1.0f, dt);
			/* the buzzing modulator is as wide as several of its harmonics: cap as if it were */
			float fbn = v->idx2 * (1.0f / IDX_SCALE);
			ramp(v, v->env * v->vel * trem, idx_cap(v, (0.3f + 1.8f * tone) * (0.4f + 0.6f * v->menv) * IDX_SCALE,
			                                       1.0f + 2.0f * fbn));
			v->bend *= dcoef(0.15f, sdt);
			float vib = 1.0f + 0.004f * sin_u(lfo_vib + (uint32_t)k * 0x40000000u);
			float hz = mtof(v->note + v->bend) * speed * pbend * vib;
			/* the buzz is a self-modulated modulator, as rich as a saw: less of it up high,
			 * where its harmonics would fold back */
			v->idx2 = fbk * clampf((3000.0f - hz) * (1.0f / 2200.0f), 0.1f, 1.0f);
			v->ic = hz_inc(hz);
			v->im = hz_inc(hz);
		}
		break;
	}
	case GV_TAILS: {
		int z = stepped(P.ch[ch][CP_CHAR], 3);
		float dec = expmap(len, 2.0f, 14.0f), tau = 0.35f * dec;
		for (int k = 0; k < 3; k++) {
			nvoice *v = &c->v[k];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			v->t += sdt;
			nv_perc(v, 0.008f, dec / speed, 1.0f, 1.0f, dt);
			/* the index tail: falls (bright to pure), blooms (rises, then falls) or rises */
			float x = clampf(v->t / tau, 0.0f, 40.0f), sh;
			if (z == 0)
				sh = fexp2(-x * 1.4427f);
			else if (z == 1)
				sh = x < 0.5f ? x * 2.0f : fexp2(-(x - 0.5f) * 1.4427f * 1.5f);
			else
				sh = 1.0f - fexp2(-x * 1.4427f);
			float im = idx_cap(v, (0.1f + 2.6f * tone) * (0.08f + 0.92f * sh) * IDX_SCALE, 1.0f);
			v->iinc = (im - v->idx) * (1.0f / VSUB);
			nv_pitch(v, v->note);
		}
		break;
	}
	case GV_KALIMBA:
	default:
		break;
	}
	c->glow *= dcoef(0.4f, dt);
}

/* ---- per sub-block: audio ---- */
/* the pulse's noise (hats, shakers, the snare's rattle), at 48 kHz: they need the top a 24 kHz
 * voice can't carry. Mono, added straight into OL and OR (GL_SUB samples) with the hats' and
 * the snare's gains each side (h, n: left, right), and into the sends at 24 kHz (E, V, scaled
 * by es and vs). (The kick and the click are in chan_render, at 24 kHz with the others.)
 *
 * White noise from the channel's own generator, through a high-pass (TONE: ~2 to ~4.5 kHz)
 * and two low-pass poles (TONE: ~14 to ~20 kHz), each with a zero at 24 kHz (a lone pole
 * there only takes 7 dB off): a hat's body in the 3-12 kHz, falling off above 14 rather than
 * stopping at 12. The snare's rattle is darker, ~0.5 to 4.5 kHz. */
static void pulse_noise(chan *c, float *OL, float *OR, const float *h, const float *n,
                        float *E, float *V, float es, float vs, int half)
{
	/* half: at 24 kHz instead (a second Pulse and on: half the cost, no top above 12 kHz),
	 * every coefficient converted so the decays and filters keep their times */
	float sa = c->ps_a, hl = c->ps_x, hp1 = c->ps_h, sy = c->ps_y, sz = c->ps_z;
	float na = c->sn_a, nm = c->sn_m, nl = c->sn_l;
	const float t = eff_tone((int)(c - CH));
	float sk = c->ps_k, nk = c->sn_k, kh = 0.25f + 0.2f * t, lk = 0.85f + 0.1f * t, kn = 0.45f, kl = 0.06f;
	uint32_t ns = GL_SUB;
	if (half) {
		sk *= sk;
		nk *= nk;
		kh = 1.0f - (1.0f - kh) * (1.0f - kh);
		lk = 1.0f - (1.0f - lk) * (1.0f - lk);
		kn = 1.0f - (1.0f - kn) * (1.0f - kn);
		kl = 1.0f - (1.0f - kl) * (1.0f - kl);
		ns = GL_SUB / 2;
	}
	const float hL = h[0], hR = h[1], nL = n[0], nR = n[1];
	const int snare = na > 1e-4f;
	uint32_t r = c->pn ? c->pn : 0x9E3779B9u;
	float acc = 0.0f;
	for (uint32_t i = 0; i < ns; i++) {
		r ^= r << 13;
		r ^= r >> 17;
		r ^= r << 5;
		float w = (float)(int32_t)r * (1.0f / 2147483648.0f);
		hl += kh * (w - hl);
		float hp = w - hl, sy1 = sy;
		sy += lk * (0.5f * (hp + hp1) - sy);
		sz += lk * (0.5f * (sy + sy1) - sz);
		hp1 = hp;
		float sh = sz * sa * 3.0f, sn = 0.0f;
		sa *= sk;
		if (snare) {
			float nm1 = nm;
			nm += kn * (w - nm);
			float nb = 0.5f * (nm + nm1);
			nl += kl * (nb - nl);
			sn = (nb - nl) * na * 1.6f;
			na *= nk;
		}
		OL[i] += sh * hL + sn * nL;
		OR[i] += sh * hR + sn * nR;
		acc += 1.7f * sh + 1.1f * sn;
		if (half) {
			E[i] += 2.0f * acc * es;
			V[i] += 2.0f * acc * vs;
			acc = 0.0f;
		} else if (i & 1) {
			E[i >> 1] += acc * es;
			V[i >> 1] += acc * vs;
			acc = 0.0f;
		}
	}
	c->pn = r;
	c->ps_a = sa; c->ps_x = hl; c->ps_h = hp1; c->ps_y = sy; c->ps_z = sz;
	c->sn_a = na; c->sn_m = nm; c->sn_l = nl;
}

/* ---- the analog voices ---- */
/* a saw with its step smoothed (PolyBLEP), so it does not alias at 24 kHz */
static inline float blep_saw(uint32_t ph, uint32_t inc)
{
	float t = (float)ph * (1.0f / 4294967296.0f), dt = (float)inc * (1.0f / 4294967296.0f);
	float s = 2.0f * t - 1.0f;
	if (t < dt) {
		float x = t / dt;
		s -= x + x - x * x - 1.0f;
	} else if (t > 1.0f - dt) {
		float x = (t - 1.0f) / dt;
		s -= x * x + x + x + 1.0f;
	}
	return s;
}
/* strings (2 saws) and super (5), through two low-pass poles */
static void nv_render_saws(nvoice *v, float *out, int five)
{
	uint32_t a = v->pc, b = v->pc2, c3 = v->pm, d = v->pc3, e = v->pc4;
	const uint32_t ia = v->ic, ib = v->ic2, ic = v->im, id = v->ic3, ie = v->ic4;
	float amp = v->amp, ai = v->ainc, f1 = v->f1, f2 = v->f2;
	const float k = v->fk;
	for (uint32_t i = 0; i < VSUB; i++) {
		float x = blep_saw(a, ia) + blep_saw(b, ib);
		a += ia;
		b += ib;
		if (five) {
			x += blep_saw(c3, ic) + 0.8f * (blep_saw(d, id) + blep_saw(e, ie));
			c3 += ic;
			d += id;
			e += ie;
		}
		f1 += k * (x - f1);
		f2 += k * (f1 - f2);
		out[i] += f2 * amp;
		amp += ai;
	}
	v->pc = a; v->pc2 = b; v->pm = c3; v->pc3 = d; v->pc4 = e;
	v->amp = amp; v->f1 = f1; v->f2 = f2;
}
/* glow: built from its first three odd harmonics (a triangle's, softened), each weighed
 * per note (fq: the third's, fk: the fifth's), so nothing above them exists to alias or bite */
static void nv_render_glow(nvoice *v, float *out)
{
	uint32_t pc = v->pc;
	const uint32_t ic = v->ic;
	float amp = v->amp, ai = v->ainc;
	const float w2 = v->idx2, w3 = v->fq, w5 = v->fk, g = v->hold > 0.0f ? v->hold : 1.0f;
	for (uint32_t i = 0; i < VSUB; i++) {
		float x = sin_u(pc) + w2 * sin_u(2u * pc) - w3 * sin_u(3u * pc) + w5 * sin_u(5u * pc);
		pc += ic;
		out[i] += x * amp * g;
		amp += ai;
	}
	v->pc = pc; v->amp = amp;
}
/* seq: a saw through a resonant state-variable low-pass */
static void nv_render_seq(nvoice *v, float *out)
{
	uint32_t pc = v->pc;
	const uint32_t ic = v->ic;
	float amp = v->amp, ai = v->ainc, lo = v->f1, bp = v->f2;
	const float f = v->fk, q = v->fq;
	for (uint32_t i = 0; i < VSUB; i++) {
		/* a saw and a square (two saws half a cycle apart) half and half: rounder */
		float x = 0.75f * blep_saw(pc, ic) - 0.35f * blep_saw(pc + 0x80000000u, ic);
		pc += ic;
		lo += f * bp;
		float hi = x - lo - q * bp;
		bp += f * hi;
		bp = clampf(bp, -4.0f, 4.0f);
		out[i] += lo * amp * 1.5f;
		amp += ai;
	}
	v->pc = pc; v->amp = amp; v->f1 = lo; v->f2 = bp;
}

/* a 24 dB low-pass: four one-poles with a little feedback (q) from the last; its states in
 * f1, f2, idx2, menv2 */
#define LADDER(x, y)                                     \
	do {                                                 \
		float u_ = (x) - q * l4;                         \
		l1 += g * (u_ - l1);                             \
		l2 += g * (l1 - l2);                             \
		l3 += g * (l2 - l3);                             \
		l4 += g * (l3 - l4);                             \
		(y) = l4 * (1.0f + 0.6f * q);                    \
	} while (0)
/* ribbon: two saws a few cents apart through the ladder */
static void nv_render_ribbon(nvoice *v, float *out)
{
	uint32_t a = v->pc, b = v->pc2;
	const uint32_t ia = v->ic, ib = v->ic2;
	float amp = v->amp, ai = v->ainc, l1 = v->f1, l2 = v->f2, l3 = v->idx2, l4 = v->menv2;
	const float g = v->fk, q = v->fq;
	for (uint32_t i = 0; i < VSUB; i++) {
		float x = 0.5f * (blep_saw(a, ia) + blep_saw(b, ib)), y;
		a += ia;
		b += ib;
		LADDER(x, y);
		out[i] += y * amp;
		amp += ai;
	}
	v->pc = a; v->pc2 = b; v->amp = amp; v->f1 = l1; v->f2 = l2; v->idx2 = l3; v->menv2 = l4;
}
/* strings: one saw a note through the ladder (the width comes from the chorus after) */
static void nv_render_str(nvoice *v, float *out)
{
	uint32_t a = v->pc;
	const uint32_t ia = v->ic;
	float amp = v->amp, ai = v->ainc, l1 = v->f1, l2 = v->f2, l3 = v->idx2, l4 = v->menv2;
	const float g = v->fk, q = v->fq;
	for (uint32_t i = 0; i < VSUB; i++) {
		float x = blep_saw(a, ia), y;
		a += ia;
		LADDER(x, y);
		out[i] += y * amp;
		amp += ai;
	}
	v->pc = a; v->amp = amp; v->f1 = l1; v->f2 = l2; v->idx2 = l3; v->menv2 = l4;
}
/* the Juno's chorus, on the strings channel: the mono strings through a short delay swept by a
 * triangle LFO (I: ~0.5 Hz, toward II: ~0.86 Hz and deeper), about 1.7 to 5.4 ms, the left and
 * right taps swept in opposite directions, each mixed with the dry. A gentle high-pass (about
 * 150 Hz) first, as the Juno's, so the chords never turn thick. The delay line is the channel's
 * string buffer, which the strings do not otherwise use */
static inline int16_t to16(float x);
/* audition strings: Solina (an 8' and a 4' saw through two poles) */
static void nv_render_sol(nvoice *v, float *out)
{
	uint32_t a = v->pc, b = v->pc2;
	const uint32_t ia = v->ic, ib = v->ic2;
	float amp = v->amp, ai = v->ainc, f1 = v->f1, f2 = v->f2;
	const float k = v->fk;
	for (uint32_t i = 0; i < VSUB; i++) {
		float x = blep_saw(a, ia) + 0.5f * blep_saw(b, ib);
		a += ia;
		b += ib;
		f1 += k * (x - f1);
		f2 += k * (f1 - f2);
		out[i] += f2 * amp;
		amp += ai;
	}
	v->pc = a; v->pc2 = b; v->amp = amp; v->f1 = f1; v->f2 = f2;
}
/* audition strings: Juno PWM (a pulse, two saws half a width apart, through the ladder) */
static void nv_render_pwm(nvoice *v, float *out)
{
	uint32_t a = v->pc;
	const uint32_t ia = v->ic, w = (uint32_t)(v->bend * 4294967296.0f);
	float amp = v->amp, ai = v->ainc, l1 = v->f1, l2 = v->f2, l3 = v->idx2, l4 = v->menv2;
	const float g = v->fk, q = v->fq;
	for (uint32_t i = 0; i < VSUB; i++) {
		float x = 0.6f * (blep_saw(a, ia) - blep_saw(a + w, ia)), y;
		a += ia;
		LADDER(x, y);
		out[i] += y * amp;
		amp += ai;
	}
	v->pc = a; v->amp = amp; v->f1 = l1; v->f2 = l2; v->idx2 = l3; v->menv2 = l4;
}
/* the Solina's ensemble: three short delay lines (about 4 ms) swept by a slow and a fast LFO,
 * each line a third of a turn from the next; left takes the first and half the second, right the
 * third and half the second, with a little of the dry. A gentle high-pass first */
static void solina_ensemble(chan *c, int ch, float *A, float *B)
{
	int16_t *rb = c->ks;
	const float chr = P.ch[ch][CP_CHAR];
	const float ctr = 96.0f, ds = (18.0f + 14.0f * chr) * speed, df = 2.2f * speed;
	uint32_t s0 = c->lfo1, f0 = c->lfo2;
	c->lfo1 += sub_inc(0.6f + 0.25f * chr, speed);
	c->lfo2 += sub_inc(5.5f, speed);
	float d[3], dd[3];
	for (int j = 0; j < 3; j++) {
		uint32_t o = (uint32_t)j * 0x55555555u;
		d[j] = ctr + ds * sin_u(s0 + o) + df * sin_u(f0 + o);
		dd[j] = (ctr + ds * sin_u(c->lfo1 + o) + df * sin_u(c->lfo2 + o) - d[j]) * (1.0f / VSUB);
	}
	float hp = c->hq_vel;
	uint32_t w = c->hq_i;
	for (uint32_t i = 0; i < VSUB; i++) {
		float x = A[i];
		hp += 0.039f * (x - hp);
		x -= hp;
		rb[w] = to16(0.5f * x);
		float t[3];
		for (int j = 0; j < 3; j++) {
			float p = (float)(w + 256u) - d[j];
			int ip = (int)p;
			float f = p - (float)ip, a0 = rb[ip & 255], a1 = rb[(ip + 1) & 255];
			t[j] = a0 + f * (a1 - a0);
			d[j] += dd[j];
		}
		A[i] = 0.3f * x + (t[0] + 0.5f * t[1]) * (0.55f / 8192.0f);
		B[i] = 0.3f * x + (t[2] + 0.5f * t[1]) * (0.55f / 8192.0f);
		w = (w + 1u) & 255u;
	}
	c->hq_vel = hp;
	c->hq_i = (uint8_t)w;
}
static void juno_chorus(chan *c, int ch, float *A, float *B)
{
	int16_t *rb = c->ks;                         /* 256 samples, 1.0 = 8192 */
	const float chr = P.ch[ch][CP_CHAR];
	const float rate = 0.51f + 0.35f * chr, lo = 40.0f, span = (90.0f + 40.0f * chr) * speed;
	/* the LFO once a block; the delays glide across it */
	float ph = c->hq_t, hp = c->hq_vel;
	float t0 = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
	ph += rate * (float)VSUB / VSR;
	ph -= ph >= 1.0f ? 1.0f : 0.0f;
	float t1 = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
	float dl = lo + span * (0.5f + 0.5f * t0), dr = lo + span * (0.5f - 0.5f * t0);
	const float dd = span * 0.5f * (t1 - t0) * (1.0f / VSUB);
	uint32_t w = c->hq_i;
	for (uint32_t i = 0; i < VSUB; i++) {
		float x = A[i];
		hp += 0.039f * (x - hp);                  /* ~150 Hz */
		x -= hp;
		rb[w] = to16(0.5f * x);
		float pl = (float)(w + 256u) - dl, pr = (float)(w + 256u) - dr;
		int il = (int)pl, ir = (int)pr;
		float fl = pl - (float)il, fr = pr - (float)ir;
		float a0 = rb[il & 255], a1 = rb[(il + 1) & 255], b0 = rb[ir & 255], b1 = rb[(ir + 1) & 255];
		A[i] = 0.7f * x + (a0 + fl * (a1 - a0)) * (0.7f / 8192.0f);
		B[i] = 0.7f * x + (b0 + fr * (b1 - b0)) * (0.7f / 8192.0f);
		dl += dd;
		dr -= dd;
		w = (w + 1u) & 255u;
	}
	c->hq_t = ph;
	c->hq_vel = hp;
	c->hq_i = (uint8_t)w;
}

/* audition glow 3: a sine (a little third) and breath: noise from its own generator through a
 * narrow band-pass at the note (ratio: its coefficient; idx2: the breath's level) */
static void nv_render_glow3(nvoice *v, float *out)
{
	uint32_t pc = v->pc, r = v->pm | 1u;
	const uint32_t ic = v->ic;
	float amp = v->amp, ai = v->ainc, lo = v->f1, bp = v->f2, nl = v->menv2;
	const float w2 = v->idx2, w3 = v->fq, br = v->bend * 2.0f, f = v->ratio;
	const float g = (alt_m[GV_GLOW] == 3 ? 0.84f : 1.0f) * (v->hold > 0.0f ? v->hold : 1.0f);
	for (uint32_t i = 0; i < VSUB; i++) {
		r ^= r << 13;
		r ^= r >> 17;
		r ^= r << 5;
		float w = (float)(int32_t)r * (1.0f / 2147483648.0f);
		nl += 0.18f * (w - nl);                  /* soft air: nothing hissy above ~700 Hz of it */
		lo += f * bp;
		float hi = 2.0f * nl - lo - 0.2f * bp;
		bp += f * hi;
		float x = sin_u(pc) + w2 * sin_u(2u * pc) - w3 * sin_u(3u * pc) + br * bp;
		pc += ic;
		out[i] += x * amp * g;
		amp += ai;
	}
	v->pc = pc; v->pm = r; v->amp = amp; v->f1 = lo; v->f2 = bp; v->menv2 = nl;
}

/* render the channel into its two sides, A and B; returns whether anything sounded */
static int chan_render(chan *c, float *A, float *B)
{
	int any = 0;
	if (!STEREO[c->voice])
		B = A;                                  /* mono voice: both sides are one buffer */
	switch (c->voice) {
	case GV_KIT:
		any = kit_render(c, A);
		break;
	case GV_PULSE:                              /* the kick and the click; its noise is in pulse_noise */
		if (c->pk_a > 1e-4f) {                    /* the kick: a sine whose pitch falls */
			any = 1;
			float kph = c->pk_ph, kf = c->pk_f, ka = c->pk_a;
			const float fend = c->pk_fend, kk = c->pk_k;
			for (uint32_t i = 0; i < VSUB; i++) {
				kph += kf * (1.0f / VSR);
				kph -= kph >= 1.0f ? 1.0f : 0.0f;
				kf += (fend - kf) * 0.0012f;
				A[i] += sin_u((uint32_t)(kph * 4294967296.0f)) * ka * 2.6f;
				ka *= kk;
			}
			c->pk_ph = kph; c->pk_f = kf; c->pk_a = ka;
		}
		if (c->pc_a > 1e-4f) {                    /* the click: ~15 ms of a high sine */
			any = 1;
			float cph = c->pc_ph, ca = c->pc_a;
			const float ci = c->pc_inc, ck = c->pc_k > 0.0f ? c->pc_k : 0.992f;
			for (uint32_t i = 0; i < VSUB; i++) {
				cph += ci;
				cph -= cph >= 1.0f ? 1.0f : 0.0f;
				B[i] += sin_u((uint32_t)(cph * 4294967296.0f)) * ca * 1.3f;
				ca *= ck;
			}
			c->pc_ph = cph; c->pc_a = ca;
		}
		break;
	case GV_PAD:
	case GV_BELLS:
		for (int i = 0; i < NV; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f)
				{
				nv_render(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_SWELL:
		for (int i = 0; i < 3; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				nv_render_dual(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_STRINGS: {
		const int m = alt_m[GV_STRINGS];
		float mid[VSUB];
		if (m == 3)
			for (uint32_t i = 0; i < VSUB; i++)
				mid[i] = 0.0f;
		for (int i = 0; i < 3; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				if (m == 0)
					nv_render_str(&c->v[i], A);
				else if (m == 1)
					nv_render_sol(&c->v[i], A);
				else if (m == 2)
					nv_render_pwm(&c->v[i], A);
				else
					nv_render_ribbon(&c->v[i], i == 0 ? A : i == 1 ? B : mid);   /* bowed: spread */
			}
		/* the ensemble or chorus runs on (a few ms of tail, and its line kept flowing) */
		if (m == 1)
			solina_ensemble(c, (int)(c - CH), A, B);
		else if (m == 3)
			for (uint32_t i = 0; i < VSUB; i++) {
				A[i] += 0.7f * mid[i];
				B[i] += 0.7f * mid[i];
			}
		else
			juno_chorus(c, (int)(c - CH), A, B);
		any = 1;
		break;
	}
	case GV_SUPER:
		for (int i = 0; i < 3; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				nv_render_saws(&c->v[i], (i & 1) ? B : A, 1);
				any = 1;
			}
		break;
	case GV_RIBBON:
		if (alt_m[GV_RIBBON] >= 2) {
			for (int i = 0; i < (alt_m[GV_RIBBON] == 3 ? 3 : 1); i++)
				if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
					nv_render(&c->v[i], A);
					any = 1;
				}
		} else if (c->v[0].amp > 0.0f || c->v[0].ainc > 0.0f) {
			nv_render_ribbon(&c->v[0], A);
			any = 1;
		}
		break;
	case GV_FUNK:
		if (c->v[0].amp > 0.0f || c->v[0].ainc > 0.0f) {
			nv_render_seq(&c->v[0], A);
			any = 1;
		}
		break;
	case GV_GLOW:
	case GV_SEQ:
		for (int i = 0; i < 3; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				if (c->voice == GV_GLOW && (alt_m[GV_GLOW] == 3 || (alt_m[GV_GLOW] == 2 && c->v[i].bend > 0.0f)))
					nv_render_glow3(&c->v[i], A);        /* (with breath in it) */
				else if (c->voice == GV_GLOW)
					nv_render_glow(&c->v[i], A);
				else
					nv_render_seq(&c->v[i], A);
				any = 1;
			}
		break;
	case GV_EP:
		for (int i = 0; i < 3; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				nv_render_ep(&c->v[i], A);
				any = 1;
			}
		break;
	case GV_SHIMMER:
		for (int i = 0; i < 4; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				nv_render_shimmer(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_GONG:
		for (int i = 0; i < 2; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				nv_render(&c->v[i], A);
				any = 1;
			}
		break;
	case GV_PIANO:
	case GV_MARIMBA:
	case GV_ARP:
	case GV_PIZZ:
	case GV_TAILS:
		for (int i = 0; i < 3; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f)
				{
				nv_render(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_DRONE:
		for (int i = 0; i < 2; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f)
				{
				nv_render(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_FLUTE:
		for (int k = 0; k < 3; k++) {
			nvoice *v = &c->v[k];
			if (!(v->amp > 0.0f || v->ainc > 0.0f))
				continue;
			any = 1;
			float *out = v->side ? B : A;
			uint32_t pc = v->pc, ic = v->ic;
			float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc, fb = v->fb;
			for (uint32_t i = 0; i < VSUB; i++) {
				float o = sin_u(pc + (uint32_t)(int32_t)(fb * idx));
				fb = 0.5f * (fb + o);              /* the DX7's two-sample average */
				pc += ic;
				out[i] += o * amp;
				amp += ai;
				idx += ii;
			}
			v->pc = pc; v->amp = amp; v->idx = idx; v->fb = fb;
		}
		break;
	case GV_BASS: {
		nvoice *v = &c->v[0];
		if (!(v->amp > 0.0f || v->ainc > 0.0f))
			break;
		any = 1;
		/* the fundamental, the sub-octave (less as TONE rises) and, as AMB-1's bass has, a little
		 * of the octave above, so it carries on small speakers; 4 dB more than before */
		float sub = v->ratio, fund = 1.0f - 0.5f * sub;
		uint32_t pc = v->pc, pm = v->pm, ic = v->ic, im = v->im;
		float amp = v->amp * 1.6f, ai = v->ainc * 1.6f, idx = v->idx, ii = v->iinc, fb = v->fb;
		for (uint32_t i = 0; i < VSUB; i++) {
			float o = sin_u(pc + (uint32_t)(int32_t)(fb * idx));
			fb = o;
			A[i] += (o * fund + SIN[pm >> TBL_SHIFT] * (0.9f * sub) + SIN[(pc << 1) >> TBL_SHIFT] * 0.15f) * amp;
			pc += ic;
			pm += im;
			amp += ai;
			idx += ii;
		}
		amp *= 1.0f / 1.6f;
		v->pc = pc; v->pm = pm; v->amp = amp; v->idx = idx; v->fb = fb;
		break;
	}
	case GV_KALIMBA:
	case GV_HARP:
	case GV_HARPSI:
		if (c->voice == GV_HARP && alt_m[GV_HARP] == 3)
			for (int i = 0; i < 4; i++)
				if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
					nv_render(&c->v[i], c->v[i].side ? B : A);
					any = 1;
				}
		for (int k = 0, nk = ks_count(c->voice); k < nk; k++) {
			int len = c->ks_len[k];
			if (!len)
				continue;
			any = 1;
			int16_t *b = c->ks + k * (KS_BUF / nk);
			int i = c->ks_i[k];
			float g = c->ks_g[k] * (1.0f / 16000.0f), a = c->ks_a[k], na = 1.0f - a;
			float *out = (k & 1) ? B : A;
			uint32_t n = 0;
			if (c->ks_fill[k]) {
				float ea = c->ks_ea[k], lp = c->ks_elp[k], vel = c->ks_vel[k];
				uint32_t m = c->ks_fill[k] < VSUB ? c->ks_fill[k] : VSUB;
				const int shaped = c->ks_hold[k] < 0.0f;
				const int at = len - (int)c->ks_fill[k];
				for (; n < m; n++) {
					float src = rnd() * 2.0f - 1.0f;     /* (drawn even when shaped: the dice stay in step) */
					if (shaped) {                        /* the string pulled at a seventh of its length */
						float q = (float)(at + (int)n) / (float)len, fsh = -c->ks_hold[k];
						float sh = (q < 0.14f ? q * (1.0f / 0.14f) : (1.0f - q) * (1.0f / 0.86f)) * 2.0f - 1.0f;
						src = fsh > 0.995f ? sh : ((1.0f - fsh) * src + fsh * 0.225f * sh) *
						      (1.0f + 0.8f * fsh * (1.0f - fsh) * 4.0f);   /* (a blend, kept as loud) */
					}
					lp += ea * (src - lp);
					float x = lp * vel;                      /* the string's units: 1.0 = 16000 */
					b[i] = (int16_t)(x * 16000.0f);
					out[n] += x * 1.8f;
					i = i + 1 == len ? 0 : i + 1;
				}
				c->ks_elp[k] = lp;
				c->ks_fill[k] = (uint16_t)(c->ks_fill[k] - m);
			}
			float y = (float)b[i];                     /* carried: one load per sample */
			if (c->voice != GV_KALIMBA) {              /* tuned: the all-pass in the loop */
				float cc = c->ks_c[k], x1 = c->ks_x1[k], y1 = c->ks_y1[k];
				for (; n < VSUB; n++) {
					int j = i + 1 == len ? 0 : i + 1;
					float yj = (float)b[j];
					float nx = (a * y + na * yj) * g;
					float ap = cc * (nx - y1) + x1;
					x1 = nx;
					y1 = ap;
					b[i] = (int16_t)(ap * 16000.0f);
					y = yj;
					i = j;
					out[n] += ap * 1.8f;
				}
				c->ks_x1[k] = x1;
				c->ks_y1[k] = y1;
			}
			for (; n < VSUB; n++) {
				int j = i + 1 == len ? 0 : i + 1;
				float yj = (float)b[j];
				float nx = (a * y + na * yj) * g;
				b[i] = (int16_t)(nx * 16000.0f);
				y = yj;
				i = j;
				out[n] += nx * 1.8f;
			}
			c->ks_i[k] = (uint16_t)i;
			/* the string's level, estimated once per block (its filter only makes it quieter
			 * sooner): below -70 dB it stops costing anything */
			c->ks_env[k] *= c->ks_kd[k];
			if (!c->ks_fill[k] && c->ks_env[k] < 0.0003f)
				c->ks_len[k] = 0;
		}
		break;
	default:
		break;
	}
	return any;
}

/* ================================================================ effects */

/* ---- the space (echo and reverbs) is in glag_space.h, included after the distortion ---- */

static inline int16_t to16(float x)
{
	x *= 16384.0f;
#if defined(__ARM_ARCH) && defined(__ARM_FEATURE_SAT)
	return (int16_t)__builtin_arm_ssat((int32_t)x, 16);    /* VCVT saturates, SSAT clamps */
#else
	return (int16_t)(x > 32767.0f ? 32767.0f : x < -32767.0f ? -32767.0f : x);
#endif
}
static inline float from16(int16_t v) { return (float)v * (1.0f / 16384.0f); }

/* ---- DJ filter: TPT state-variable, stereo ---- */
static float svf_s[2][2], svf_a1, svf_a2, svf_a3, svf_k = 1.2f;
static float svf_mode;                         /* <0 low-pass, >0 high-pass, 0 through */
static float dc_x[2], dc_y[2];                 /* DC blocker (feedback FM leaves an offset) */

static void filter_setup(float v)
{
	float fc;
	if (v < 0.47f) {
		fc = 18000.0f * fexp2(-(0.47f - v) / 0.47f * 9.0f);
		svf_mode = -1.0f;
	} else if (v > 0.53f) {
		fc = 18.0f * fexp2((v - 0.53f) / 0.47f * 9.6f);
		svf_mode = 1.0f;
	} else {
		fc = 18000.0f;
		svf_mode = 0.0f;
	}
	/* g = tan(pi fc / SR), from the sine table */
	uint32_t ph = (uint32_t)(fc * (0.5f / GL_SR) * 4294967296.0f);
	float g = sin_u(ph) / sin_u(ph + 0x40000000u);
	svf_k = svf_mode < 0.0f ? 1.1f : 1.3f;
	svf_a1 = 1.0f / (1.0f + g * (g + svf_k));
	svf_a2 = g * svf_a1;
	svf_a3 = g * svf_a2;
}

/* derived from the patch when it changes, not every millisecond (see derive()) */
static float d_drift = 60.0f;
static float k_freeze;
static void derive(void);
static void space_init(void);
static void space_settle(void);

/* ================================================================ main thread API */

void gl_init(uint32_t seed)
{
	for (int i = 0; i <= TBL; i++) {
		/* sine without libm: a short Taylor series per point, then exact quarter symmetry */
		float x = (float)(i % (TBL / 2)) * (6.2831853f / TBL);
		float y = x > 1.5707963f ? 3.1415927f - x : x, y2 = y * y;
		float s = y * (1.0f - y2 / 6.0f * (1.0f - y2 / 20.0f * (1.0f - y2 / 42.0f * (1.0f - y2 / 72.0f * (1.0f - y2 / 110.0f)))));
		SIN[i] = (i % TBL) < TBL / 2 ? s : -s;
	}
	SIN[TBL] = SIN[0];
	rng = seed ? seed : 0x9E3779B9u;
	gl_piece(0, &P);
	post_buf[0] = post_buf[1] = P;
	space_init();
	mode_cur = root_cur = -1;
	chord_deg = 0;
	harmony_update();
	chord_build();
	chord_pending = 0;
	for (int c = 0; c < GL_NCH; c++) {
		chan_reset(&CH[c], P.voice[c]);
		if (CH[c].voice == GV_BELLS)
			bells_fill(&CH[c], c);
	}
	derive();
	space_settle();                        /* the tape at its length: no slide at the start */
}

void gl_crank(void) { cmd_crank++; }

void gl_fresh(uint32_t seed)
{
	cmd_fresh_seed = seed;
	cmd_fresh++;
}

void gl_post_patch(const gl_patch *p)
{
	uint32_t s = post_seq + 1u;
	post_buf[s & 1u] = *p;
	BARRIER();
	post_seq = s;
}
void gl_play(int on) { cmd_play = (uint8_t)(on != 0); }
void gl_freeze(int on) { cmd_freeze = (uint8_t)(on != 0); }
void gl_slow(int on) { cmd_slow = (uint8_t)(on != 0); }
void gl_next_chord(void) { cmd_next++; }
void gl_panic(void) { cmd_panic++; }

/* ================================================================ distortion
 * Five cheap waveshapers, a few operations a sample each. Driven harder at each amount, then
 * brought back down so a step changes the colour more than the loudness. */
const char *const GL_DIST_NAME[GD_COUNT] = {"Chebyshev", "Overdrive", "Fuzz", "XOR"};
static volatile uint8_t cmd_dist_type, cmd_dist_amt, cmd_dist_post;
void gl_dist(int type, int amount, int post)
{
	cmd_dist_type = (uint8_t)((unsigned)type < GD_COUNT ? type : 0);
	cmd_dist_amt = (uint8_t)(amount < 0 ? 0 : amount > 4 ? 4 : amount);
	cmd_dist_post = (uint8_t)(post < 0 ? 0 : post > 3 ? 3 : post);
}

static struct {
	uint8_t type, amt, post;
	float g, mix, mk;          /* drive, how much of the shaped signal, make-up gain */
	int32_t prev[10];          /* XOR: the last crushed sample per path (L, R, echo, reverb, wet L, wet R, the drums' four) */
	float env[10];             /* Chebyshev: each path's level */
	float c2, c3;              /* Chebyshev: how much octave (T2) and twelfth (T3) */
} dist;

static void dist_setup(void)
{
	static const float DRIVE[GD_COUNT][4] = {
		{1.2f, 1.6f, 2.2f, 3.0f},      /* Chebyshev: T2 and T3 harmonics growing */
		{2.0f, 3.5f, 6.0f, 11.0f},     /* overdrive: soft saturation */
		{3.0f, 6.0f, 12.0f, 25.0f},    /* fuzz: biased hard clip */
		{1.0f, 1.0f, 1.0f, 1.0f},      /* XOR: crushed and XORed bits */
	};
	static const float MAKEUP[GD_COUNT][4] = {
		{1.209f, 1.524f, 1.677f, 1.446f},
		{0.42f, 0.307f, 0.238f, 0.188f},
		{0.162f, 0.111f, 0.093f, 0.087f},
		{1.333f, 1.952f, 2.26f, 1.839f},
	};
	dist.type = cmd_dist_type;
	dist.amt = cmd_dist_amt;
	dist.post = cmd_dist_post;
	fm_dist = FM_CAP - (dist.amt ? FM_DIST_CUT : 0);
	fm_cap = fm_dist < gov_cap ? fm_dist : gov_cap;
	static const float C2[4] = {0.12f, 0.22f, 0.35f, 0.5f}, C3[4] = {0.15f, 0.3f, 0.5f, 0.75f};
	if (dist.amt) {
		dist.c2 = C2[dist.amt - 1];
		dist.c3 = C3[dist.amt - 1];
		dist.g = DRIVE[dist.type][dist.amt - 1];
		dist.mk = MAKEUP[dist.type][dist.amt - 1];
		dist.mix = 0.35f + 0.65f * (float)dist.amt * 0.25f;
	}
}

/* shape a buffer in place; `path` keeps each signal's own memory (L, R, echo, reverb). One
 * loop per type with everything in locals: the type is decided once per buffer, not per sample. */
static void dist_block(float *x, uint32_t n, int path)
{
	const float mk = dist.mk, g = dist.g;
	switch (dist.type) {
	case GD_CHEBYSHEV: {
		/* A harmonic exciter: the signal is brought to full scale by its own level (T_n makes
		 * clean harmonics only there), T2 adds the octave and T3 the twelfth, and the level is
		 * put back. T2's offset is taken out by the output's high-pass. */
		/* c2 T2(u) + c3 T3(u) = 4 c3 u^3 + 2 c2 u^2 - 3 c3 u - c2, in Horner form */
		/* the level first, so the signal never passes full scale (that would be a hard clip):
		 * this block's peak, or the last level falling over ~50 ms, whichever is higher */
		float pk = 0.0f;
		for (uint32_t i = 0; i < n; i++) {
			float a = x[i] > 0.0f ? x[i] : -x[i];
			pk = a > pk ? a : pk;
		}
		float d = dist.env[path] * 0.98f, e = pk > d ? pk : d;
		dist.env[path] = e;
		if (e < 0.0005f)
			break;                                      /* silence: nothing to excite */
		const float inv = 1.0f / e;
		const float pa = 4.0f * dist.c3 * mk, pb = 2.0f * dist.c2 * mk, pc = -3.0f * dist.c3 * mk, pd = -dist.c2 * mk;
		for (uint32_t i = 0; i < n; i++) {
			float v = x[i], u = v * inv;
			x[i] = v * mk + e * (((pa * u + pb) * u + pc) * u + pd);
		}
		break;
	}
	case GD_OVERDRIVE: {
		const float k = mk * 1.6f;
		for (uint32_t i = 0; i < n; i++) {
			float v = x[i] * g;
			x[i] = v / (1.0f + (v > 0.0f ? v : -v)) * k;
		}
		break;
	}
	case GD_FUZZ: {
		const float k = mk * 2.2f;
		for (uint32_t i = 0; i < n; i++)                /* biased: uneven halves, a buzz */
			x[i] = (clampf(x[i] * g + 0.15f, -0.55f, 0.85f) - 0.15f) * k;
		break;
	}
	default: {
		/* crush to fewer bits, then XOR with the last crushed sample shifted down */
		static const uint8_t BITS[4] = {11, 9, 7, 5};
		static const float XMIX[4] = {0.2f, 0.4f, 0.65f, 0.9f};
		const int sh = 16 - BITS[dist.amt - 1];
		const float xm = XMIX[dist.amt - 1];
		const float wet = xm * mk * (1.0f / 32768.0f) * (float)(1 << sh), dk = (1.0f - xm) * mk;
		int32_t prev = dist.prev[path];
		for (uint32_t i = 0; i < n; i++) {
			int32_t q = (int32_t)(clampf(x[i], -1.0f, 1.0f) * 32767.0f) >> sh;
			int32_t y = q ^ (prev >> 1);
			prev = q;
			x[i] = x[i] * dk + (float)y * wet;
		}
		dist.prev[path] = prev;
		break;
	}
	}
}

static void reverse_block(float *WL, float *WR, uint32_t n);
#include "glag_space.h"

/* ================================================================ render */

static float lvl_ch[GL_NCH];

static void derive(void)
{
	const float dt = (float)GL_SUB / GL_SR;
	float bpm = gl_tempo_bpm(P.g[GP_TEMPO]);
	d_drift = expmap(1.0f - P.g[GP_DRIFT], 10.0f, 240.0f);
	space_derive(bpm);
	k_freeze = 1.0f - dcoef(0.08f, dt);
	filter_setup(P.g[GP_FILTER]);
}

static void macro_tick(float dt)
{
	static uint8_t was_on[GL_NCH];
	for (int c = 0; c < GL_NCH; c++) {
		const uint8_t *w = MW[CH[c].voice];
		float d = mac_depth[c];
		if (d <= 0.0f) {                      /* off: put the controls back once, then cost nothing */
			if (was_on[c]) {
				for (int p = 0; p < GL_NCP; p++)
					P.ch[c][p] = Pb.ch[c][p];
				was_on[c] = 0;
			}
			continue;
		}
		was_on[c] = 1;
		if (mac_shape[c] == 1) {
			/* steps: a new value for each on every bar, jumped to (a few ms of smoothing) */
			static uint32_t bar_seen[GL_NCH];
			if (step_n / 16u != bar_seen[c]) {
				bar_seen[c] = step_n / 16u;
				mac_to[c][0] = 2.0f * rnd() - 1.0f;
				mac_to[c][1] = 2.0f * rnd() - 1.0f;
			}
			for (int k = 0; k < 2; k++)
				mac_u[c][k] += (mac_to[c][k] - mac_u[c][k]) * 0.2f;
		} else if (mac_shape[c] == 2) {
			/* a long rise: from the bottom to the top over six glides, then back down in one */
			for (int k = 0; k < 2; k++) {
				mac_ph[c][k] += dt / (mac_period[c] * mac_pmul[c] * 7.0f * (k ? 1.1f : 1.0f));
				mac_ph[c][k] -= mac_ph[c][k] >= 1.0f ? 1.0f : 0.0f;
				float x = mac_ph[c][k] * 7.0f / 6.0f;
				mac_u[c][k] = x <= 1.0f ? -1.0f + 2.0f * x * x : 1.0f - 2.0f * (x - 1.0f) * 6.0f;
			}
		} else
		for (int k = 0; k < 2; k++) {
			/* a cosine glide from one random value to the next, once per (slightly uneven) period */
			mac_ph[c][k] += dt / (mac_period[c] * mac_pmul[c] * (k ? 1.37f : 1.0f));
			if (mac_ph[c][k] >= 1.0f) {
				mac_ph[c][k] -= 1.0f;
				mac_from[c][k] = mac_to[c][k];
				mac_to[c][k] = 2.0f * rnd() - 1.0f;
			}
			float x = 0.5f - 0.5f * sin_u((uint32_t)(mac_ph[c][k] * 2147483648.0f) + 0x40000000u);
			mac_u[c][k] = mac_from[c][k] + (mac_to[c][k] - mac_from[c][k]) * x;
		}
		float u1 = mac_u[c][0], u2 = mac_u[c][1];
		d *= 1.0f + d;                       /* gentle at the first steps, twice as wide at full */
		P.ch[c][CP_TONE] = clampf(Pb.ch[c][CP_TONE] + d * w[0] * 0.006f * u1, 0.0f, 1.0f);
		if (CH[c].voice == GV_PULSE) {
			/* its Character is two things: rings (lower half) or the 4/4 grid (upper), and the
			 * swing within each. The macro stays in the half it found and only swings it, gently
			 * until the last step (the depth squared): a beat that shuffles only when you ask */
			float b = Pb.ch[c][CP_CHAR], lo = b <= 0.5f ? 0.0f : 0.51f, hi = b <= 0.5f ? 0.5f : 1.0f;
			P.ch[c][CP_CHAR] = clampf(b + d * mac_depth[c] * w[1] * 0.005f * u2, lo, hi);
		} else
			P.ch[c][CP_CHAR] = clampf(Pb.ch[c][CP_CHAR] + d * w[1] * 0.007f * u2, 0.0f, 1.0f);
		P.ch[c][CP_LENGTH] = clampf(Pb.ch[c][CP_LENGTH] + d * w[4] * 0.005f * u2, 0.0f, 1.0f);
		P.ch[c][CP_ACTIVITY] = clampf(Pb.ch[c][CP_ACTIVITY] + d * w[5] * 0.005f * u1, 0.0f, 1.0f);
		mac_trem_ph[c] += dt * (3.0f + 2.0f * u2);   /* 1..5 Hz, wandering */
		if (mac_trem_ph[c] >= 1.0f)
			mac_trem_ph[c] -= 1.0f;
	}
}
static inline float macro_trem(int c)
{
	if (mac_depth[c] <= 0.0f)
		return 1.0f;
	float d = mac_depth[c] * (1.0f + mac_depth[c]) * 0.5f;
	float t = d * MW[CH[c].voice][2] * 0.01f;
	return 1.0f - t * 0.5f * (1.0f + sin_u((uint32_t)(mac_trem_ph[c] * 4294967295.0f)));
}

#include "glag_play.h"

/* one sixteenth of the clock: the bar's chord, every channel's step (play: 1; 2, the crank's,
 * leaves the drums out: they add little to a frozen space and a tap on a hat is only a click) */
static void clock_step(int play)
{
	if ((step_n & 15) == 0 && chord_pending == 2) {   /* drifted: on the bar */
		chord_pending = 0;
		chord_next();
	}
	if (play)
		for (int c = 0; c < GL_NCH; c++)
			if (!CH[c].fading && !(play == 2 && (CH[c].voice == GV_PULSE || CH[c].voice == GV_KIT))) {
				/* its time division: every step, or one in 2, 3 or 4, its patterns spread out */
				uint32_t dv = GL_CHX_DIV(P.chx[c]) + 1u;
				if (step_n % dv == 0u) {
					if (midi_live(c))
						;                                   /* MIDI plays it */
					else if (arp_live(c))
						arp_step(&CH[c], c, step_n / dv);   /* arp mode plays it */
					else
						chan_step(&CH[c], c, step_n / dv);
				}
			}
	if (play == 1)
		pump_step(step_n);                    /* Playworld's pump on its own clock */
	if ((step_n & 15u) == 0u)
		arp_bar();                            /* arp mode on or off: on the bar */
	step_n++;
}

static void control(float dt)
{
	uint32_t s = post_seq;
	if (cmd_fresh != seen_fresh) {
		/* the album: this piece from its start, exactly as rendered: the state of power-on with
		 * this seed (piece 1 loaded, its chord built, every channel reset, the space emptied),
		 * then the posted piece taken on top of it, as the renders take theirs */
		seen_fresh = cmd_fresh;
		rng = cmd_fresh_seed ? cmd_fresh_seed : 0x9E3779B9u;
		step_n = 0;
		step_acc = 0.0f;
		chord_timer = 0.0f;
		chord_moves = 0;
		key_off = 0;
		mode_swap = 0;
		speed = 1.0f;
		arc = 0.0f;
		lfo_vib = lfo_trem = lfo_arc1 = lfo_arc2 = 0;
		for (int c = 0; c < GL_NCH; c++)
			for (int k = 0; k < 2; k++)
				mac_ph[c][k] = mac_from[c][k] = mac_to[c][k] = mac_u[c][k] = 0.0f;
		gl_piece(0, &P);
		space_init();
		mode_cur = root_cur = -1;
		chord_deg = 0;
		harmony_update();
		chord_build();
		chord_pending = 0;
		for (int c = 0; c < GL_NCH; c++) {
			chan_reset(&CH[c], P.voice[c]);
			if (CH[c].voice == GV_BELLS)
				bells_fill(&CH[c], c);
		}
		derive();
		space_settle();
		post_seen = s - 1u;                       /* and now the piece itself, as posted */
	}
	if (s != post_seen) {
		P = post_buf[s & 1u];
		Pb = P;
		post_seen = s;
		harmony_update();
		derive();
	}
	playing = cmd_play;
	int freeze_changed = frozen != cmd_freeze;
	frozen = cmd_freeze;
	if (cmd_dist_type != dist.type || cmd_dist_amt != dist.amt || cmd_dist_post != dist.post)
		dist_setup();
	slowing = cmd_slow;
	macro_tick(dt);
	midi_tick(dt);
	world_tick();
	form_tick(dt);
	key_tick(dt);
	autof_tick();
	if (cmd_panic != seen_panic) {
		seen_panic = cmd_panic;
		for (int c = 0; c < GL_NCH; c++) {
			chan_reset(&CH[c], P.voice[c]);
			if (CH[c].voice == GV_BELLS)
				bells_fill(&CH[c], c);
		}
	}
	if (cmd_next != seen_next) {
		seen_next = cmd_next;
		chord_pending = 1;
	}

	/* the slowdown: down to 10 BPM over 10 s, then back with a spring */
	float bpm = gl_tempo_bpm(P.g[GP_TEMPO]);
	float floor_ = 10.0f / bpm;
	if (slowing) {
		speed -= (1.0f - floor_) * dt / 10.0f;
		if (speed < floor_)
			speed = floor_;
	} else if (speed < 1.0f) {
		speed += (1.0f - speed) * (1.0f - dcoef(0.35f, dt)) + dt * 0.2f;
		if (speed > 1.0f)
			speed = 1.0f;
	}
	gl_speed = speed;
	lfo_vib += sub_inc(5.1f, speed);
	lfo_trem += sub_inc(7.3f, speed);
	lfo_arc1 += sub_inc(1.0f / 170.0f, speed);
	lfo_arc2 += sub_inc(1.0f / 47.0f, speed);

	/* the arc: a slow tide between sparse and full, as deep as MOTION */
	arc = P.g[GP_MOTION] * (0.65f * sin_u(lfo_arc1) + 0.35f * sin_u(lfo_arc2));

	/* chord drift: 10 s .. 4 min */
	if (!frozen && playing)
		chord_timer += dt * speed;
	if (chord_timer >= d_drift) {
		chord_timer = 0.0f;
		chord_pending = 2;
	}
	if (chord_pending == 1) {                  /* asked for: now */
		chord_pending = 0;
		chord_next();
	}

	/* voice changes: fade the channel out, swap, fade in */
	for (int c = 0; c < GL_NCH; c++) {
		chan *ch = &CH[c];
		if (P.voice[c] < GV_COUNT && P.voice[c] != ch->voice && !ch->fading)
			ch->fading = 1;
		if (ch->fading) {
			ch->fade -= dt / 0.03f;
			if (ch->fade <= 0.0f) {
				chan_reset(ch, P.voice[c] < GV_COUNT ? P.voice[c] : GV_PAD);
				if (ch->voice == GV_BELLS)
					bells_fill(ch, c);
			}
		} else if (ch->fade < 1.0f) {
			ch->fade += dt / 0.03f;
			if (ch->fade > 1.0f)
				ch->fade = 1.0f;
		}
	}

	/* the clock: sixteenths, slowed with everything else */
	step_acc += dt * speed * bpm * (4.0f / 60.0f);
	gl_beat = (float)(step_n & 3) * 0.25f + step_acc * 0.25f;
	while (step_acc >= 1.0f) {
		step_acc -= 1.0f;
		clock_step(playing && !frozen);
	}
	/* the crank (RWD on Home while resting or frozen): on to the next step where something
	 * plays, a bar at most, played now */
	while (cmd_crank != seen_crank) {
		seen_crank++;
		crank_open = 0.5f;                    /* frozen: the space takes this note in, then holds again */
		uint32_t before = notes_fired;
		for (int k = 0; k < 16 && notes_fired == before; k++)
			clock_step(2);                    /* (not the drums) */
	}
	for (int c = 0; c < GL_NCH; c++) {
		chan_control(&CH[c], c, dt);
		gl_glow[c] = CH[c].glow;
		float l = P.ch[c][CP_LEVEL];
		lvl_ch[c] = l * l * CH[c].fade * w_lvl[c];
	}

	/* the space (the patch-dependent parts are in derive()) */
	space_control(dt, freeze_changed);
	if (crank_open > 0.0f)
		crank_open -= dt;
	tp_hold = frozen && crank_open <= 0.0f;      /* the echo's loop held, as the reverb is */
	freeze_in += ((frozen && crank_open <= 0.0f ? 0.0f : 1.0f) - freeze_in) * k_freeze;
	gl_playing = playing;
	gl_frozen = frozen;
}

#if VOICE_HALF
/* the upsampler: a 12-tap half-band (Kaiser-windowed sinc), 27 dB down by 15 kHz and 42 by 16,
 * where the 4-point it replaces managed 13 and 16: the images of bright partials no longer ring */
static float hb_l[11], hb_r[11];
static const float HB[6] = {0.619623f, -0.166023f, 0.062933f, -0.021032f, 0.004843f, -0.000343f};
#endif
static float vd_s[2];                     /* the dark send */
static float pu_l[GL_SUB], pu_r[GL_SUB];   /* the pulse's noise, at 48 kHz, added after the upsampling */
static int pu_on;

void gl_render(int16_t *lr, uint32_t n, float gain)
{
	const float dt = (float)GL_SUB / GL_SR;
	float peak = gl_peak * 0.9f;
	for (uint32_t base = 0; base + GL_SUB <= n; base += GL_SUB) {
		control(dt);
		/* The instruments at 24 kHz: the dry mix (HL, HR) and the sends. Plucked and struck
		 * voices go into the reverb bright; held ones go in through a dark send (AMB-1 darkens its
		 * pad the same way), and less of them the higher they sit above C4. */
		float L[GL_SUB], R[GL_SUB], E[GL_SUB / 2], V[GL_SUB / 2], VD[GL_SUB / 2], HL[VSUB], HR[VSUB];
		for (uint32_t i = 0; i < GL_SUB / 2; i++)
			E[i] = V[i] = VD[i] = 0.0f;
		for (uint32_t i = 0; i < VSUB; i++)
			HL[i] = HR[i] = 0.0f;
		/* the sends run at 24 kHz: at full rate (a measuring build), pairs are averaged */
#define SI(i) ((i) >> (1 - VOICE_HALF))
		const float sk = VOICE_HALF ? 1.0f : 0.5f;
		int pulses = 0;
		const float pg = pump_gain(dt, gl_tempo_bpm(P.g[GP_TEMPO]) * speed);   /* Playworld's pump */
		sp_duck = (1.0f - W.pspace * (1.0f - pg)) * form_space;
		for (int c = 0; c < GL_NCH; c++) {
			/* the drums may be taken out while the pump goes on; bass and held voices spared */
			int ro = voice_role(CH[c].voice);
			float pc = ro == ROLE_BASS || ro == ROLE_HELD ? 1.0f - (1.0f - pg) * (1.0f - W.pspare) : pg;
			float g = lvl_ch[c] * 0.5f * macro_trem(c) * (CH[c].voice == GV_PULSE || CH[c].voice == GV_KIT ? W.pdr_s : 1.0f);
			if (CH[c].voice == GV_PULSE || CH[c].voice == GV_KIT)
				pc = 1.0f;
			if (g <= 0.0f)
				continue;                   /* silent: its envelopes still run in control() */
			int vo = CH[c].voice, st = STEREO[vo];
			if (vo == GV_PULSE && (CH[c].ps_a > 1e-4f || CH[c].sn_a > 1e-4f)) {
				/* its noise at 48 kHz, added in after the others come up (with distortion
				 * before the effects, in with them at 24 kHz instead, to be distorted); its
				 * kick and click go on below, at 24 kHz. Its two sides: the hats 1 and 0.7,
				 * the snare's rattle 0.55 each, either side of the pan. (The sends: the pair's
				 * sum, as twice the average a stereo voice sends.) */
				float pan = P.ch[c][CP_PAN], sp = SPREAD[vo];
				float p0 = clampf(pan - sp, 0.0f, 1.0f), p1 = clampf(pan + sp, 0.0f, 1.0f);
				float h[2] = {((1.0f - p0) + 0.7f * (1.0f - p1)) * g, (p0 + 0.7f * p1) * g};
				float nn[2] = {0.55f * ((1.0f - p0) + (1.0f - p1)) * g, 0.55f * (p0 + p1) * g};
				float es = P.ch[c][CP_ECHO] * g, vs = P.ch[c][CP_VERB] * g;
				/* the rule for more than one: the first Pulse's noise at 48 kHz, the rest at
				 * 24 kHz (half the cost each; four at once would overrun otherwise). With
				 * distortion before the effects, all at 24 kHz, in with the others */
				if (dist.amt && dist.post == 3) {
					/* the drums only: the noise shaped too, before its sends, so its reverb
					 * carries it shaped */
					float tl[GL_SUB], tr[GL_SUB], e0[GL_SUB / 2], v0[GL_SUB / 2];
					for (uint32_t i = 0; i < GL_SUB; i++)
						tl[i] = tr[i] = 0.0f;
					for (uint32_t i = 0; i < GL_SUB / 2; i++)
						e0[i] = v0[i] = 0.0f;
					pulse_noise(&CH[c], tl, tr, h, nn, e0, v0, 0.0f, 0.0f, 0);
					dist_block(tl, GL_SUB, 8);
					dist_block(tr, GL_SUB, 9);
					for (uint32_t i = 0; i < GL_SUB; i += 2) {
						float m = (tl[i] + tl[i + 1] + tr[i] + tr[i + 1]) / (g > 1e-6f ? g : 1.0f);
						E[i >> 1] += m * es;
						V[i >> 1] += m * vs;
					}
					for (uint32_t i = 0; i < GL_SUB; i++) {
						pu_l[i] += tl[i];
						pu_r[i] += tr[i];
					}
					pu_on = 1;
				} else if (VOICE_HALF && (pulses++ > 0 || (dist.amt && dist.post == 0)))
					pulse_noise(&CH[c], HL, HR, h, nn, E, V, es, vs, 1);
				else {
					pulse_noise(&CH[c], pu_l, pu_r, h, nn, E, V, es, vs, 0);
					pu_on = 1;
				}
			}
			float ca[VSUB], cb[VSUB];
			for (uint32_t i = 0; i < VSUB; i++)   /* (not memset: the M4's is byte by byte) */
				ca[i] = 0.0f;
			if (st)
				for (uint32_t i = 0; i < VSUB; i++)
					cb[i] = 0.0f;
			if (!chan_render(&CH[c], ca, cb)) {
				pump_prev[c] = pc;
				continue;
			}
			if (pc < 1.0f || pump_prev[c] < 1.0f) {
				/* the pump's duck, gliding sample by sample from the last block's: a duck in
				 * steps of a millisecond would click once the drums no longer cover it */
				float p0 = pump_prev[c], pd = (pc - p0) * (1.0f / VSUB);
				for (uint32_t i = 0; i < VSUB; i++) {
					p0 += pd;
					ca[i] *= p0;
					if (st)
						cb[i] *= p0;
				}
			}
			pump_prev[c] = pc;
			if ((vo == GV_PULSE || vo == GV_KIT) && dist.amt && dist.post == 3) {   /* the drums only: kick and click */
				dist_block(ca, VSUB, 6);
				dist_block(cb, VSUB, 7);
			}
			float pan = P.ch[c][CP_PAN], es = P.ch[c][CP_ECHO] * g, vs = P.ch[c][CP_VERB] * g;
			float *VV = V;
			if (!PLUCK[vo]) {
				int hn = HOME[vo] + 12 * (stepped(P.ch[c][CP_REGISTER], 5) - 2);
				if (hn > 60)
					vs *= fexp2(-(float)(hn - 60) * (1.0f / 24.0f));   /* -3 dB an octave */
				VV = VD;
			}
			if (!st) {
				float al = (1.0f - pan) * g, ar = pan * g;
				for (uint32_t i = 0; i < VSUB; i++) {
					float a0 = ca[i];
					HL[i] += a0 * al;
					HR[i] += a0 * ar;
					E[SI(i)] += 4.0f * sk * a0 * es;   /* as loud in the sends as a stereo voice */
					VV[SI(i)] += 4.0f * sk * a0 * vs;
				}
				continue;
			}
			/* the two sides, spread either side of the channel's pan */
			float sp = SPREAD[vo];
			float pa = clampf(pan - sp, 0.0f, 1.0f), pb = clampf(pan + sp, 0.0f, 1.0f);
			float al = (1.0f - pa) * g, ar = pa * g, bl = (1.0f - pb) * g, br = pb * g;
			for (uint32_t i = 0; i < VSUB; i++) {
				float a0 = ca[i], b0 = cb[i];
				HL[i] += a0 * al + b0 * bl;
				HR[i] += a0 * ar + b0 * br;
				float m = 2.0f * sk * (a0 + b0);
				E[SI(i)] += m * es;
				VV[SI(i)] += m * vs;
			}
		}
		texture_block(HL, HR, VSUB, VSR);           /* Playworld's bed, dry */
		{	/* the dark send: 12 dB an octave from 1.5 kHz */
			const float k = 0.3245f;              /* 1 - exp(-2 pi 1500 / 24000) */
			float d1 = vd_s[0], d2 = vd_s[1];
			for (uint32_t i = 0; i < GL_SUB / 2; i++) {
				d1 += k * (VD[i] - d1);
				d2 += k * (d1 - d2);
				V[i] += d2;
			}
			vd_s[0] = d1;
			vd_s[1] = d2;
		}
		/* distortion before the effects: the dry mix and what goes into them, at 24 kHz. The
		 * middle of the image is distorted and the sides left clean: half the work, and
		 * instruments panned wide keep their spread. */
		if (dist.amt && dist.post == 0) {
			float M[VSUB];
			for (uint32_t i = 0; i < VSUB; i++)
				M[i] = 0.5f * (HL[i] + HR[i]);
			dist_block(M, VSUB, 0);
			for (uint32_t i = 0; i < VSUB; i++) {
				float sd = 0.5f * (HL[i] - HR[i]);
				HL[i] = M[i] + sd;
				HR[i] = M[i] - sd;
			}
			dist_block(E, GL_SUB / 2, 2);
			dist_block(V, GL_SUB / 2, 3);
		}
		/* back up to 48 kHz: each sample, and a half-band midpoint before it */
#if !VOICE_HALF
		for (uint32_t i = 0; i < GL_SUB; i++) {
			L[i] = HL[i];
			R[i] = HR[i];
		}
#else
		{	/* the last 11 samples and this block's 24, one window: no shifting */
			float wl[11 + VSUB], wr[11 + VSUB];
			for (int k = 0; k < 11; k++) {
				wl[k] = hb_l[k];
				wr[k] = hb_r[k];
			}
			for (uint32_t i = 0; i < VSUB; i++) {
				wl[11 + i] = HL[i];
				wr[11 + i] = HR[i];
			}
			for (uint32_t i = 0; i < VSUB; i++) {
				const float *a = wl + i, *b = wr + i;      /* a[0..11]: the 12 around this midpoint */
				L[2 * i] = HB[0] * (a[5] + a[6]) + HB[1] * (a[4] + a[7]) + HB[2] * (a[3] + a[8]) +
				           HB[3] * (a[2] + a[9]) + HB[4] * (a[1] + a[10]) + HB[5] * (a[0] + a[11]);
				R[2 * i] = HB[0] * (b[5] + b[6]) + HB[1] * (b[4] + b[7]) + HB[2] * (b[3] + b[8]) +
				           HB[3] * (b[2] + b[9]) + HB[4] * (b[1] + b[10]) + HB[5] * (b[0] + b[11]);
				L[2 * i + 1] = a[6];
				R[2 * i + 1] = b[6];
			}
			for (int k = 0; k < 11; k++) {
				hb_l[k] = wl[VSUB + k];
				hb_r[k] = wr[VSUB + k];
			}
		}
#endif
		if (pu_on) {
			for (uint32_t i = 0; i < GL_SUB; i++) {
				L[i] += pu_l[i];
				R[i] += pu_r[i];
				pu_l[i] = pu_r[i] = 0.0f;
			}
			pu_on = 0;
		}
		/* the space, at half rate; its output interpolated back up (and, with distortion on
		 * the reverb only, that happens in there) */
		space_block(E, V, L, R);
		/* ...or after them, on everything (the DJ filter below can tame it) */
		if (dist.amt && dist.post == 1) {
			dist_block(L, GL_SUB, 0);
			dist_block(R, GL_SUB, 1);
		}
		warble_block(L, R, GL_SUB);                  /* Playworld: the vinyl's warble */
		/* DJ filter, DC blocker, soft clip, out */
		float a1 = svf_a1, a2 = svf_a2, a3 = svf_a3, k = svf_k, md = svf_mode;
		float s0l = svf_s[0][0], s1l = svf_s[0][1], s0r = svf_s[1][0], s1r = svf_s[1][1];
		float xl = dc_x[0], yl = dc_y[0], xr = dc_x[1], yr = dc_y[1];
		if (md != 0.0f) {
			for (uint32_t i = 0; i < GL_SUB; i++) {
				float l = L[i], r = R[i];
				float v3 = l - s1l, v1 = a1 * s0l + a2 * v3, v2 = s1l + a2 * s0l + a3 * v3;
				s0l = 2.0f * v1 - s0l;
				s1l = 2.0f * v2 - s1l;
				v3 = r - s1r;
				float w1 = a1 * s0r + a2 * v3, w2 = s1r + a2 * s0r + a3 * v3;
				s0r = 2.0f * w1 - s0r;
				s1r = 2.0f * w2 - s1r;
				L[i] = md < 0.0f ? v2 : l - k * v1 - v2;
				R[i] = md < 0.0f ? w2 : r - k * w1 - w2;
			}
		}
		/* the master compressor (glue): both sides' peak, caught in 10 ms and let go over 250 ms,
		 * read once a millisecond; above the threshold (-10 to -22 dBFS) the ratio 1.5:1 to 6:1,
		 * a soft knee, a quarter of what it takes back made up. Its gain glides across the
		 * millisecond, so it costs a multiply a sample */
		float cg = comp_g, cgi = 0.0f;
		if (comp_amt > 0.0f || comp_g != 1.0f) {
			float env = comp_env;
			for (uint32_t i = 0; i < GL_SUB; i += 4) {
				float d = L[i] > 0.0f ? L[i] : -L[i], e = R[i] > 0.0f ? R[i] : -R[i];
				d = (d > e ? d : e) * gain;
				env = d > env ? env + 0.0083f * (d - env) : env * 0.99967f;
			}
			comp_env = env;
			float tgt = 1.0f;
			if (comp_amt > 0.0f) {
				float thr = -10.0f - 12.0f * comp_amt, ratio = 1.5f + 4.5f * comp_amt, knee = 6.0f;
				float over = 6.0206f * flog2(env + 1e-6f) - thr, gr;
				gr = over <= -knee ? 0.0f : over >= knee ? over : (over + knee) * (over + knee) / (4.0f * knee);
				gr *= 1.0f - 1.0f / ratio;
				float mk = -thr * (1.0f - 1.0f / ratio) * 0.25f;
				tgt = fexp2((mk - gr) * 0.16610f);
			}
			cgi = (tgt - cg) * (1.0f / GL_SUB);
			comp_g = tgt;
		}
		int16_t *o = lr + 2u * base;
		for (uint32_t i = 0; i < GL_SUB; i++) {
			float l = L[i], r = R[i];
			yl = l - xl + 0.9987f * yl; xl = l;      /* ~10 Hz high-pass */
			yr = r - xr + 0.9987f * yr; xr = r;
			l = clampf(yl * gain * cg, -1.0f, 1.0f);
			r = clampf(yr * gain * cg, -1.0f, 1.0f);
			cg += cgi;
			l *= 1.5f - 0.5f * l * l;                 /* soft clip, 1.0 at full scale */
			r *= 1.5f - 0.5f * r * r;
			o[2u * i] = (int16_t)(l * 32000.0f);
			o[2u * i + 1u] = (int16_t)(r * 32000.0f);
		}
		for (uint32_t i = 0; i < GL_SUB; i += 8) {   /* a peak for the LEDs, sampled */
			float a = L[i] > 0.0f ? L[i] : -L[i];
			if (a * gain > peak)
				peak = a * gain;
		}
		svf_s[0][0] = s0l; svf_s[0][1] = s1l; svf_s[1][0] = s0r; svf_s[1][1] = s1r;
		dc_x[0] = xl; dc_y[0] = yl; dc_x[1] = xr; dc_y[1] = yr;
	}
	gl_peak = peak;
}

/* ================================================================ pieces */

/* Sixteen recipes. Values are percent (0..100). Columns per channel:
 *   ACT TONE LEN REG ECHO VERB PAN CHAR LEVEL      REG: 0,25,50,75,100 = -2..+2 octaves
 * Globals: TEMPO DELAY REVERB FILTER ROOT(0..11) MODE(0..7) DRIFT MOTION */
typedef struct {
	const char *name;
	uint8_t voice[GL_NCH];
	uint8_t ch[GL_NCH][GL_NCP];
	uint8_t g[GL_NGP];
	uint8_t space[3];          /* reverb, shimmer interval, echo time (GL_ECHO_NAME) */
	uint8_t mac[GL_NCH];       /* the channel macros on load, in steps (0 off .. 4 full) */
	uint8_t mac_s;             /* their glide, in seconds (0: the usual 8) */
	uint8_t scene[GL_NCH];     /* the hidden macro scene (•• + FFWD held 3 s on Home brings it in) */
	uint8_t scene_s;
	uint8_t flags;             /* space[GL_SP_FLAGS]: reverse, ping-pong, key travel, macro shape */
	uint8_t chx[GL_NCH];       /* per channel: time division and interval */
} recipe;

static const recipe PIECES[GL_NPIECES] = {
	/* ---- after AMB-1's eight stock pieces (Charles Vestal), in Glag's voices: its pad, bells,
	 * bass and pulse are channels 1 to 4, its numbers mapped onto Glag's controls (the pad's
	 * breaths as long as AMB-1's: LENGTH = 0.12 x Swell / 26) ---- */
	/* Garden: everything, gently */
	{"Garden", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 40, 28, 50, 27, 65, 50, 20, 60}, {40, 50, 55, 50, 45, 65, 50, 25, 65},
	  {35, 30, 85, 50, 9, 26, 50, 35, 45}, {35, 50, 40, 50, 14, 26, 50, 0, 40}},
	 {52, 45, 68, 50, 2, 1, 57, 52}, {GR_HALL, GS_OCTAVE, 3}, {0, 0, 0, 0}, 0, {2, 2, 1, 1}, 12, 0, {0, 0, 0, 0}},
	/* Departures: slowed bells an octave down over a quiet pad, as 1/1 */
	{"Departures", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 20, 25, 50, 18, 70, 50, 20, 35}, {35, 14, 55, 25, 30, 70, 50, 2, 75},
	  {35, 30, 85, 50, 6, 28, 50, 25, 0}, {0, 40, 40, 50, 9, 28, 50, 0, 0}},
	 {40, 30, 77, 50, 1, 0, 44, 45}, {GR_BLOOM, GS_OCTAVE, 3}, {0, 0, 0, 0}, 0, {2, 3, 0, 0}, 16, 0, {0, 0, 0, 0}},
	/* Voices: only the sung pad on unequal loops and a low bass, as 2/1 */
	{"Voices", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 67, 32, 50, 12, 80, 50, 70, 85}, {30, 34, 55, 50, 20, 80, 50, 15, 0},
	  {35, 30, 85, 25, 4, 32, 50, 30, 25}, {0, 40, 40, 50, 6, 32, 50, 0, 0}},
	 {36, 20, 85, 50, 5, 5, 36, 52}, {GR_SHIMMER, GS_FIFTH, 4}, {0, 0, 0, 0}, 0, {3, 0, 1, 0}, 16, 0, {0, 0, 0, 0}},
	/* Afternoon: a bright pad, busier bells, a light pulse */
	{"Afternoon", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 60, 23, 50, 30, 60, 50, 20, 50}, {55, 58, 55, 50, 50, 60, 50, 30, 65},
	  {35, 30, 85, 50, 10, 24, 50, 40, 35}, {30, 65, 40, 50, 15, 24, 50, 8, 25}},
	 {64, 50, 60, 50, 7, 3, 61, 60}, {GR_HALL, GS_OCTAVE, 2}, {0, 0, 0, 0}, 0, {1, 3, 1, 2}, 8, 0, {0, 0, 0, 0}},
	/* Night Bus: the pulse forward, metallic bells, short echoes */
	{"Night Bus", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 35, 18, 50, 33, 55, 50, 20, 45}, {45, 70, 55, 50, 55, 55, 50, 100, 55},
	  {35, 30, 85, 50, 11, 22, 50, 45, 55}, {55, 70, 40, 50, 16, 22, 50, 15, 60}},
	 {76, 55, 51, 50, 9, 5, 65, 70}, {GR_CHAMBER, GS_OCTAVE, 1}, {0, 0, 0, 0}, 0, {1, 2, 1, 3}, 6, 0, {0, 0, 0, 0}},
	/* Shoreline: dark, round bells lost in long echoes, as 2/2 */
	{"Shoreline", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 30, 37, 50, 51, 55, 50, 20, 40}, {30, 10, 55, 50, 85, 55, 50, 0, 60},
	  {35, 30, 85, 50, 17, 22, 50, 20, 30}, {0, 30, 40, 50, 26, 22, 50, 0, 0}},
	 {44, 85, 94, 50, 4, 6, 44, 72}, {GR_BLOOM, GS_OCTAVE, 4}, {0, 0, 0, 0}, 0, {2, 3, 1, 0}, 12, 0, {0, 0, 0, 0}},
	/* Glass: clangy bells an octave up, dense */
	{"Glass", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 55, 21, 50, 27, 65, 50, 20, 35}, {60, 82, 55, 75, 45, 65, 50, 45, 80},
	  {35, 30, 85, 75, 9, 26, 50, 30, 30}, {25, 85, 40, 50, 14, 26, 50, 5, 20}},
	 {56, 45, 68, 50, 3, 7, 57, 62}, {GR_SHIMMER, GS_OCTAVE, 2}, {0, 0, 0, 0}, 0, {1, 4, 1, 2}, 6, 0, {0, 0, 0, 0}},
	/* Tides: the most changeable */
	{"Tides", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 60, 28, 50, 30, 65, 50, 70, 55}, {45, 54, 55, 50, 50, 65, 50, 28, 60},
	  {35, 30, 85, 50, 10, 26, 50, 55, 40}, {40, 50, 40, 50, 15, 26, 50, 5, 35}},
	 {50, 50, 77, 50, 10, 4, 70, 95}, {GR_HALL, GS_OCTAVE, 3}, {0, 0, 0, 0}, 0, {4, 4, 4, 4}, 10, 0, {0, 0, 0, 0}},
	/* ---- Glag's own ---- */
	{"Gate Seven", {GV_PAD, GV_PIANO, GV_DRONE, GV_BELLS},
	 {{40, 30, 75, 50, 20, 80, 50, 10, 60}, {35, 35, 60, 50, 55, 70, 45, 0, 65},
	  {40, 35, 30, 50, 0, 60, 50, 0, 45}, {25, 30, 70, 75, 50, 75, 50, 25, 50}},
	 {40, 50, 75, 50, 0, 0, 25, 40}, {GR_BLOOM, GS_OCTAVE, 3}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	{"Long Wait", {GV_DRONE, GV_FLUTE, GV_PAD, GV_KALIMBA},
	 {{30, 45, 50, 50, 10, 70, 50, 0, 60}, {25, 35, 80, 50, 60, 80, 40, 40, 55},
	  {30, 25, 90, 75, 10, 80, 50, 60, 45}, {30, 40, 40, 50, 60, 60, 65, 50, 45}},
	 {30, 55, 85, 50, 9, 5, 15, 55}, {GR_BLOOM, GS_OCTAVE, 4}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	{"Night Flight", {GV_PAD, GV_KALIMBA, GV_FLUTE, GV_DRONE},
	 {{45, 20, 85, 25, 20, 85, 50, 0, 60}, {40, 30, 60, 50, 65, 70, 35, 30, 50},
	  {20, 30, 90, 50, 60, 85, 65, 60, 50}, {25, 20, 40, 25, 0, 50, 50, 0, 40}},
	 {32, 60, 80, 50, 4, 7, 20, 45}, {GR_BLOOM, GS_OCTAVE, 3}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	{"Lounge", {GV_PIANO, GV_MARIMBA, GV_PAD, GV_FLUTE},
	 {{45, 30, 70, 50, 40, 60, 40, 0, 60}, {25, 25, 45, 75, 50, 70, 65, 70, 40},
	  {35, 40, 70, 25, 20, 70, 50, 30, 55}, {25, 50, 60, 75, 55, 75, 50, 50, 45}},
	 {42, 45, 70, 50, 5, 0, 30, 50}, {GR_CHAMBER, GS_OCTAVE, 3}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	{"Rain City", {GV_SWELL, GV_EP, GV_GONG, GV_SHIMMER},
	 {{45, 55, 75, 25, 25, 85, 50, 60, 65}, {35, 25, 55, 50, 50, 65, 40, 40, 60},
	  {30, 45, 70, 50, 10, 70, 50, 25, 55}, {25, 50, 60, 50, 70, 80, 60, 50, 45}},
	 {37, 60, 85, 50, 4, 5, 30, 55}, {GR_SHIMMER, GS_FIFTH, 3}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	{"Deep Water", {GV_SHIMMER, GV_TAILS, GV_GONG, GV_PAD},
	 {{35, 45, 70, 50, 75, 85, 40, 30, 55}, {35, 55, 70, 50, 50, 75, 60, 50, 55},
	  {25, 40, 80, 50, 15, 80, 50, 75, 50}, {40, 25, 85, 25, 15, 85, 50, 20, 50}},
	 {28, 65, 90, 50, 2, 1, 25, 60}, {GR_SHIMMER, GS_OCTAVE, 4}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	{"Lobby 84", {GV_EP, GV_PIZZ, GV_BASS, GV_SWELL},
	 {{45, 75, 55, 50, 40, 55, 45, 70, 75}, {45, 45, 30, 50, 45, 45, 60, 50, 70},
	  {40, 40, 50, 50, 5, 20, 50, 30, 65}, {40, 45, 60, 50, 20, 70, 50, 40, 60}},
	 {60, 40, 55, 50, 5, 0, 35, 45}, {GR_CHAMBER, GS_OCTAVE, 1}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	{"Long Haul", {GV_PIZZ, GV_TAILS, GV_SWELL, GV_GONG},
	 {{55, 40, 25, 75, 60, 55, 40, 100, 70}, {30, 50, 75, 50, 40, 75, 60, 50, 68},
	  {40, 40, 80, 25, 20, 80, 50, 70, 62}, {25, 45, 75, 50, 10, 70, 50, 0, 65}},
	 {50, 55, 75, 50, 9, 7, 40, 60}, {GR_HALL, GS_OCTAVE, 3}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
#ifdef GL_DEMO_PIECES
	/* ---- audition only: six in styles (not in the firmware yet) ---- */
	/* Berlin school: three Arp loops against each other in sixteenths, on the chord's root,
	 * fifth, seventh and octave: 16 steps low (the bass sequence), 7 in the middle, 5 up high
	 * in eighths; a choir pad; dotted-quarter echoes; the chord moving every few bars */
	{"Sequencer Dawn", {GV_ARP, GV_ARP, GV_ARP, GV_PAD},
	 {{92, 30, 25, 0, 30, 35, 45, 67, 74}, {90, 45, 30, 50, 45, 50, 30, 83, 46},
	  {70, 55, 40, 75, 60, 60, 70, 100, 38}, {30, 30, 85, 50, 10, 75, 50, 70, 50}},
	 {98, 45, 70, 50, 4, 5, 20, 70}, {GR_HALL, GS_OCTAVE, 0}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	/* krautrock: a straight beat (kick on 1 and 3, snare on 2 and 4, soft dark eighth hats),
	 * an eighth-note pedal bass (the Arp's pedal pattern, low), an organ drone, a jangle in
	 * eighths up high, one chord */
	{"Motorik", {GV_PULSE, GV_ARP, GV_DRONE, GV_PIZZ},
	 {{30, 30, 20, 50, 0, 15, 50, 52, 66}, {72, 25, 30, 0, 0, 10, 50, 50, 82},
	  {40, 45, 50, 50, 10, 45, 45, 0, 50}, {60, 50, 30, 75, 40, 45, 70, 0, 48}},
	 {98, 25, 35, 50, 2, 4, 5, 40}, {GR_CHAMBER, GS_OCTAVE, 1}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	/* Tangerine Dream's slow space: dark, climbing */
	{"Dark Orbit", {GV_PAD, GV_TAILS, GV_SHIMMER, GV_GONG},
	 {{50, 30, 80, 25, 20, 80, 50, 40, 60}, {35, 50, 75, 50, 50, 75, 60, 50, 50},
	  {30, 45, 60, 25, 60, 80, 40, 40, 40}, {30, 40, 80, 50, 15, 70, 50, 50, 55}},
	 {36, 55, 85, 50, 2, 2, 25, 75}, {GR_SHIMMER, GS_FIFTH, 4}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	/* city pop: a root-and-octave bass in eighths (Pizz, low), four on the floor with the
	 * snare on 2 and 4 and sixteenth hats, string-and-brass swells on major sevenths, Rhodes
	 * fills up high; the chords moving every bar or two */
	{"Night Drive", {GV_PIZZ, GV_PULSE, GV_SWELL, GV_EP},
	 {{62, 45, 35, 0, 0, 10, 50, 50, 78}, {80, 45, 25, 50, 0, 15, 50, 58, 56},
	  {50, 60, 55, 50, 15, 50, 40, 50, 56}, {60, 60, 45, 75, 30, 45, 65, 50, 62}},
	 {84, 25, 45, 50, 5, 0, 75, 40}, {GR_CHAMBER, GS_OCTAVE, 1}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	/* lofi: dark Rhodes, a soft round bass, boom-bap drums swung hard, kalimba, the DJ
	 * filter half closed */
	{"Study Lamp", {GV_EP, GV_BASS, GV_PULSE, GV_KALIMBA},
	 {{45, 22, 70, 50, 35, 45, 45, 20, 72}, {70, 15, 55, 50, 0, 10, 50, 20, 70},
	  {60, 35, 45, 50, 10, 20, 50, 85, 62}, {30, 30, 50, 50, 45, 50, 65, 35, 40}},
	 {56, 35, 40, 36, 10, 1, 100, 35}, {GR_CHAMBER, GS_OCTAVE, 2}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	/* experimental: glass bells in sixteenths, wandering intervals, octave leaps, all moving */
	{"Static Garden", {GV_BELLS, GV_DRONE, GV_TAILS, GV_PIZZ},
	 {{30, 70, 50, 75, 60, 65, 40, 100, 50}, {40, 60, 50, 25, 10, 60, 50, 100, 45},
	  {45, 60, 55, 50, 55, 65, 60, 100, 50}, {40, 50, 30, 75, 55, 55, 65, 100, 40}},
	 {52, 60, 75, 50, 1, 7, 80, 95}, {GR_BLOOM, GS_OCTAVE, 2}, {0, 0, 0, 0}, 0, {0, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	/* ---- audition only: built around the channel macros at full ---- */
	/* four plucks whose metal, octave and density never sit still: glass in a kaleidoscope */
	{"Kaleidoscope", {GV_BELLS, GV_MARIMBA, GV_KALIMBA, GV_PIZZ},
	 {{60, 50, 45, 75, 35, 55, 30, 50, 66}, {60, 50, 40, 50, 30, 50, 70, 50, 72},
	  {55, 50, 50, 50, 40, 55, 45, 50, 66}, {55, 50, 30, 75, 40, 50, 60, 30, 55}},
	 {52, 40, 65, 50, 7, 3, 40, 60}, {GR_HALL, GS_OCTAVE, 1}, {4, 4, 4, 4}, 6, {4, 4, 4, 4}, 6, 0, {0, 0, 0, 0}},
	/* held sounds turning slowly through brightness, detune and breath, like weather */
	{"Weather", {GV_PAD, GV_SWELL, GV_DRONE, GV_GONG},
	 {{40, 45, 85, 50, 15, 80, 40, 50, 58}, {35, 50, 70, 50, 20, 70, 60, 50, 50},
	  {40, 45, 60, 25, 10, 60, 50, 40, 45}, {30, 45, 80, 25, 15, 70, 50, 50, 45}},
	 {22, 35, 85, 50, 2, 1, 30, 60}, {GR_BLOOM, GS_OCTAVE, 3}, {4, 4, 4, 4}, 10, {4, 4, 4, 4}, 10, 0, {0, 0, 0, 0}},
	/* fast glides: the Rhodes' tremolo, the flute's vibrato and the piano's tine all wobbling,
	 * a warped tape */
	{"Wobble Room", {GV_EP, GV_PIANO, GV_FLUTE, GV_TAILS},
	 {{70, 40, 50, 50, 35, 45, 40, 50, 78}, {55, 40, 55, 50, 30, 50, 60, 50, 68},
	  {45, 45, 50, 50, 35, 55, 55, 50, 55}, {45, 50, 60, 25, 45, 60, 50, 50, 55}},
	 {44, 50, 55, 40, 10, 5, 50, 50}, {GR_CHAMBER, GS_OCTAVE, 2}, {4, 4, 4, 4}, 4, {4, 4, 4, 4}, 4, 0, {0, 0, 0, 0}},
	/* Sequencer Dawn's loops left to wander: the macros push the Arps between their loops and
	 * patterns, the bass between root and fifth, the pulse's hits and swing */
	{"Drift Sequencer", {GV_ARP, GV_ARP, GV_BASS, GV_PULSE},
	 {{85, 35, 25, 0, 30, 35, 45, 67, 70}, {75, 50, 30, 75, 50, 55, 65, 83, 45},
	  {20, 30, 85, 50, 0, 20, 50, 40, 60}, {45, 40, 30, 50, 10, 20, 50, 60, 50}},
	 {94, 40, 60, 50, 9, 5, 25, 60}, {GR_HALL, GS_OCTAVE, 0}, {4, 4, 2, 3}, 12, {4, 4, 2, 3}, 12, 0, {0, 0, 0, 0}},
	/* quick glides on everything: euclidean hits thinning and crowding, buzzing swoops,
	 * bells leaping octaves over a breathing pad */
	{"Glitch Choir", {GV_PULSE, GV_SHIMMER, GV_BELLS, GV_PAD},
	 {{55, 50, 35, 50, 20, 25, 50, 30, 66}, {50, 50, 45, 50, 40, 60, 35, 50, 52},
	  {55, 50, 40, 75, 40, 50, 65, 75, 55}, {35, 40, 80, 50, 10, 70, 50, 50, 55}},
	 {70, 45, 60, 50, 4, 7, 50, 70}, {GR_SHIMMER, GS_FIFTH, 1}, {0, 0, 0, 0}, 3, {4, 4, 4, 4}, 3, 0, {0, 0, 0, 0}},
	/* ---- audition only: nods to five records, with the new modes ---- */
	/* Autechre, Amber (1994): loop-built and simple, warm and melancholy. A detuned pad, a
	 * seven-step Arp loop as the lead, soft swung euclidean drums, a low bass; tape echo
	 * ping-ponging; macros breathing on pad and lead */
	{"Amber", {GV_PAD, GV_ARP, GV_PULSE, GV_BASS},
	 {{35, 40, 80, 50, 15, 75, 45, 70, 53}, {70, 30, 40, 50, 45, 50, 65, 83, 48},
	  {35, 30, 30, 50, 15, 25, 50, 30, 46}, {20, 25, 85, 50, 0, 20, 50, 40, 50}},
	 {72, 40, 60, 50, 5, 1, 30, 55}, {GR_HALL, GS_OCTAVE, 0}, {2, 3, 1, 0}, 12, {3, 4, 2, 1}, 10,
	 GL_FLAG_PINGPONG, {0, 0, 0, 0}},
	/* Aphex Twin, Selected Ambient Works II, "Blue Calx": beatless, its melodic highlight. A dark
	 * pad, slow bells on the beat, a drone a fifth, tails; Bloom with reverse swells */
	{"Blue Calx", {GV_PAD, GV_BELLS, GV_DRONE, GV_TAILS},
	 {{35, 35, 85, 50, 15, 80, 50, 60, 67}, {45, 40, 60, 75, 45, 70, 40, 20, 61},
	  {40, 35, 60, 25, 10, 60, 50, 0, 55}, {30, 40, 70, 50, 50, 70, 60, 50, 49}},
	 {28, 45, 80, 50, 1, 5, 20, 65}, {GR_BLOOM, GS_OCTAVE, 1}, {1, 2, 0, 2}, 16, {3, 3, 2, 4}, 16,
	 GL_FLAG_REVERSE, {0, 0, 0, 0}},
	/* SAW II again, "standing in a power station on acid": a drone in octaves, gongs, a deep
	 * pad, buzzing swoops, at 30 BPM in Phrygian; reverse swells and macros on a long rise */
	{"Power Station", {GV_DRONE, GV_GONG, GV_PAD, GV_SHIMMER},
	 {{50, 55, 60, 25, 10, 60, 50, 67, 60}, {30, 40, 85, 25, 20, 75, 40, 70, 55},
	  {40, 25, 90, 25, 10, 80, 55, 80, 55}, {25, 45, 50, 50, 40, 70, 60, 60, 39}},
	 {10, 45, 90, 50, 2, 2, 15, 75}, {GR_BLOOM, GS_OCTAVE, 4}, {2, 2, 2, 2}, 24, {4, 4, 4, 4}, 24,
	 GL_FLAG_REVERSE | (2u << 5), {0, 0, 0, 0}},
	/* Michael Nyman, Drowning by Numbers, "Trysting Fields": all of it from Mozart's slow movement
	 * (K.364, C minor). Staccato strings in eighths (Pizz), an up-down Arp a third above them
	 * (Nyman's parallel thirds), sustained strings and saxes (Swell), a bass leaning on the fifth;
	 * the chords moving every bar or two */
	{"Trysting Fields", {GV_PIZZ, GV_ARP, GV_SWELL, GV_BASS},
	 {{62, 55, 25, 50, 10, 30, 35, 0, 79}, {70, 45, 35, 50, 15, 35, 65, 17, 56},
	  {45, 55, 60, 50, 15, 50, 50, 55, 62}, {30, 30, 70, 50, 0, 20, 50, 70, 65}},
	 {56, 25, 45, 50, 0, 5, 65, 35}, {GR_CHAMBER, GS_OCTAVE, 5}, {0, 0, 0, 0}, 8, {1, 1, 2, 1}, 8,
	 0, {0, 1u << 2, 0, 0}},
	/* Steve Reich, Music for 18 Musicians: a steady pulse in mallets and pianos, voices and
	 * clarinets swelling for a breath; chords moving by re-voicing and to their relative minor.
	 * Two Arp loops of 7 and 5 against each other in sixteenths, the second a third above (they
	 * phase), pulsing piano eighths (Pizz, low), breaths (Swell); key travel: relative */
	{"Eighteen", {GV_ARP, GV_ARP, GV_PIZZ, GV_SWELL},
	 {{88, 45, 25, 50, 10, 30, 35, 83, 67}, {88, 50, 25, 75, 10, 30, 65, 100, 55},
	  {70, 40, 25, 25, 5, 25, 50, 0, 67}, {50, 45, 70, 50, 10, 45, 50, 40, 64}},
	 {100, 15, 40, 50, 9, 0, 35, 30}, {GR_CHAMBER, GS_OCTAVE, 5}, {1, 1, 0, 1}, 8, {2, 2, 1, 3}, 8,
	 (3u << 2) | (1u << 5), {0, 1u << 2, 0, 0}},
	/* ---- audition only: the four new voices, in several octaves ---- */
	/* Juno strings in the middle, a Glow lead an octave up, a round bass, soft swung rings */
	{"Juno Hours", {GV_STRINGS, GV_GLOW, GV_BASS, GV_PULSE},
	 {{35, 45, 50, 50, 15, 60, 50, 30, 66}, {45, 45, 55, 75, 40, 55, 60, 30, 60},
	  {20, 25, 80, 50, 0, 20, 50, 40, 60}, {30, 30, 30, 50, 10, 20, 50, 28, 51}},
	 {64, 35, 55, 50, 4, 1, 45, 50}, {GR_CHAMBER, GS_OCTAVE, 1}, {2, 3, 0, 0}, 10, {3, 4, 1, 1}, 10, 0, {0, 0, 0, 0}},
	/* two Glows two octaves apart, the high one a fifth above, a pad and a kalimba between */
	{"Glow Garden", {GV_GLOW, GV_GLOW, GV_PAD, GV_KALIMBA},
	 {{40, 40, 70, 25, 30, 55, 35, 20, 77}, {40, 55, 60, 100, 45, 65, 65, 60, 56},
	  {30, 35, 85, 50, 10, 75, 50, 60, 63}, {35, 50, 50, 75, 40, 55, 45, 50, 60}},
	 {40, 45, 70, 50, 9, 3, 30, 60}, {GR_BLOOM, GS_OCTAVE, 2}, {2, 3, 0, 1}, 12, {3, 4, 2, 2}, 12,
	 GL_FLAG_PINGPONG, {0, 2u << 2, 0, 0}},
	/* the Seq galloping low across three octaves, Juno strings high, a bass, a 4/4 pulse */
	{"Neon Ladder", {GV_SEQ, GV_STRINGS, GV_BASS, GV_PULSE},
	 {{75, 50, 40, 25, 35, 40, 45, 67, 57}, {30, 50, 55, 75, 20, 60, 55, 60, 47},
	  {25, 30, 80, 50, 0, 20, 50, 50, 51}, {55, 40, 25, 50, 10, 20, 50, 60, 47}},
	 {90, 40, 50, 50, 0, 5, 40, 55}, {GR_HALL, GS_OCTAVE, 5}, {3, 1, 0, 1}, 8, {4, 2, 1, 2}, 8, 0, {0, 0, 0, 0}},
	/* the supersaw low and slow, a Glow above it, a drone, gongs: heavy, for a big moment */
	{"Supernova", {GV_SUPER, GV_GLOW, GV_DRONE, GV_GONG},
	 {{35, 45, 70, 25, 15, 70, 50, 50, 69}, {35, 50, 60, 75, 45, 65, 60, 40, 64},
	  {40, 40, 60, 25, 10, 60, 50, 0, 55}, {30, 40, 80, 25, 20, 75, 45, 60, 61}},
	 {30, 45, 80, 50, 2, 5, 25, 70}, {GR_BLOOM, GS_OCTAVE, 3}, {3, 2, 0, 1}, 16, {4, 3, 2, 2}, 16,
	 GL_FLAG_REVERSE | (2u << 5), {0, 0, 0, 0}},
	/* strings two octaves apart (the high ones a third above), a Glow in the middle, bells */
	{"Strings at Night", {GV_STRINGS, GV_STRINGS, GV_GLOW, GV_BELLS},
	 {{30, 35, 60, 25, 10, 55, 40, 20, 73}, {30, 55, 50, 75, 20, 65, 60, 60, 54},
	  {40, 45, 55, 50, 35, 55, 50, 40, 61}, {35, 45, 55, 75, 40, 60, 55, 20, 51}},
	 {46, 40, 65, 50, 5, 0, 35, 55}, {GR_HALL, GS_OCTAVE, 1}, {2, 2, 2, 1}, 12, {3, 3, 3, 2}, 12, 0, {0, 1u << 2, 0, 0}},
	/* ---- audition only: the new voices, after the artists we keep coming back to ---- */
	/* Eno (Thursday Afternoon, Music for Airports): a slow sparse Glow up high, rare Juno chords
	 * ringing out, a pad, kalimba; Bloom and two-bar echoes */
	{"Thursday", {GV_GLOW, GV_STRINGS, GV_PAD, GV_KALIMBA},
	 {{30, 35, 85, 75, 40, 70, 40, 15, 72}, {20, 35, 90, 50, 25, 70, 55, 25, 62},
	  {30, 30, 85, 50, 10, 80, 50, 60, 57}, {30, 45, 60, 75, 45, 60, 60, 50, 54}},
	 {24, 50, 80, 50, 2, 3, 20, 60}, {GR_BLOOM, GS_OCTAVE, 4}, {2, 1, 0, 1}, 16, {3, 2, 1, 2}, 16, 0, {0, 0, 0, 0}},
	/* Tangerine Dream (Rubycon, Phaedra): the Seq bouncing octaves low in sixteenths, a slow
	 * supersaw underneath, Juno chords on the half bar, a drone; its macro on a long rise */
	{"Rubycon", {GV_SEQ, GV_SUPER, GV_STRINGS, GV_DRONE},
	 {{88, 45, 35, 25, 35, 40, 40, 0, 63}, {30, 35, 75, 25, 15, 70, 50, 45, 46},
	  {45, 40, 50, 50, 25, 55, 60, 40, 46}, {40, 40, 60, 25, 10, 60, 50, 0, 42}},
	 {84, 40, 70, 50, 4, 2, 30, 65}, {GR_HALL, GS_OCTAVE, 0}, {3, 1, 1, 0}, 12, {4, 2, 2, 1}, 12,
	 2u << 5, {0, 0, 0, 0}},
	/* Autechre (Amber, Tri Repetae): a melancholy Glow, the Seq walking in eighths, swung
	 * euclidean rings, Juno chords on the beat; ping-pong */
	{"Repetae", {GV_GLOW, GV_SEQ, GV_PULSE, GV_STRINGS},
	 {{45, 45, 60, 50, 40, 50, 40, 40, 73}, {70, 40, 40, 50, 35, 40, 60, 100, 65},
	  {40, 30, 30, 50, 15, 25, 50, 30, 65}, {65, 40, 35, 50, 20, 50, 55, 30, 58}},
	 {76, 40, 55, 50, 5, 1, 35, 55}, {GR_CHAMBER, GS_OCTAVE, 0}, {2, 2, 1, 1}, 10, {3, 3, 2, 2}, 10,
	 GL_FLAG_PINGPONG, {0, 0, 0, 0}},
	/* Aphex Twin (Avril 14th, SAW): a near-sine Glow an octave up, Juno chords on the bar
	 * ringing long, a dark pad, tails; reverse swells */
	{"Avril", {GV_GLOW, GV_STRINGS, GV_PAD, GV_TAILS},
	 {{50, 40, 65, 75, 30, 55, 45, 0, 61}, {25, 35, 85, 50, 20, 60, 55, 20, 51},
	  {30, 30, 85, 25, 10, 80, 50, 60, 47}, {30, 40, 70, 50, 45, 65, 60, 50, 41}},
	 {36, 40, 70, 50, 0, 0, 30, 55}, {GR_HALL, GS_OCTAVE, 2}, {1, 2, 0, 2}, 14, {2, 3, 1, 3}, 14,
	 GL_FLAG_REVERSE, {0, 0, 0, 0}},
	/* Nyman and Reich: chugging Juno chords on every eighth, the Seq in broken thirds in
	 * sixteenths, a Glow a third above interlocking, a bass on the fifth */
	{"Chamber Pulse", {GV_STRINGS, GV_SEQ, GV_GLOW, GV_BASS},
	 {{88, 45, 25, 50, 10, 35, 40, 40, 60}, {88, 45, 30, 50, 15, 30, 60, 33, 47},
	  {60, 40, 45, 75, 20, 40, 55, 40, 45}, {35, 30, 70, 50, 0, 20, 50, 70, 58}},
	 {98, 20, 40, 50, 7, 5, 60, 35}, {GR_CHAMBER, GS_OCTAVE, 5}, {0, 1, 1, 0}, 8, {1, 2, 2, 1}, 8, 0, {0, 0, 1u << 2, 0}},
	/* lofi, after Study Lamp: Juno chords on the beat, a Glow, a round bass, boom-bap drums
	 * swung hard, the DJ filter half closed */
	{"Juno Lamp", {GV_STRINGS, GV_GLOW, GV_BASS, GV_PULSE},
	 {{62, 30, 40, 50, 30, 45, 45, 30, 60}, {35, 35, 55, 75, 40, 50, 60, 30, 46},
	  {70, 15, 55, 50, 0, 10, 50, 20, 64}, {60, 35, 45, 50, 10, 20, 50, 85, 55}},
	 {56, 35, 40, 36, 10, 1, 70, 35}, {GR_CHAMBER, GS_OCTAVE, 2}, {1, 2, 0, 0}, 10, {2, 3, 1, 1}, 10, 0, {0, 0, 0, 0}},
	/* ---- audition only: more uses for the new voices ---- */
	/* dub techno: Juno chord stabs syncopated by their macro, thrown into ping-pong echoes, a
	 * deep bass, a soft four-on-the-floor, a dark pad */
	{"Dub Stabs", {GV_STRINGS, GV_BASS, GV_PULSE, GV_PAD},
	 {{65, 30, 15, 50, 65, 45, 50, 40, 59}, {30, 20, 70, 50, 0, 15, 50, 30, 57},
	  {80, 30, 25, 50, 5, 20, 50, 56, 48}, {25, 25, 85, 25, 15, 70, 50, 60, 37}},
	 {100, 60, 55, 50, 5, 5, 25, 40}, {GR_CHAMBER, GS_OCTAVE, 0}, {3, 0, 0, 1}, 8, {4, 1, 1, 2}, 8,
	 GL_FLAG_PINGPONG, {0, 0, 0, 0}},
	/* synthwave: a supersaw bed, the Seq bouncing octaves in sixteenths, a Glow lead, four on
	 * the floor with a snare on 2 and 4 */
	{"Outrun", {GV_SUPER, GV_SEQ, GV_GLOW, GV_PULSE},
	 {{35, 45, 70, 50, 15, 55, 50, 55, 63}, {90, 50, 35, 25, 25, 35, 45, 0, 77},
	  {50, 50, 55, 75, 35, 50, 60, 70, 69}, {80, 45, 25, 50, 5, 15, 50, 56, 74}},
	 {80, 35, 55, 50, 9, 5, 45, 45}, {GR_HALL, GS_OCTAVE, 0}, {1, 2, 2, 0}, 8, {2, 3, 3, 1}, 8, 0, {0, 0, 0, 0}},
	/* Vangelis (Blade Runner): a low brassy supersaw, a Glow lead with vibrato above, high
	 * bells, rare Juno chords; a long hall */
	{"Tears in Rain", {GV_SUPER, GV_GLOW, GV_BELLS, GV_STRINGS},
	 {{30, 50, 80, 25, 20, 75, 50, 60, 63}, {40, 50, 70, 75, 45, 70, 55, 90, 63},
	  {30, 45, 70, 75, 40, 70, 40, 75, 48}, {15, 40, 85, 50, 25, 65, 60, 50, 52}},
	 {40, 50, 88, 50, 2, 5, 25, 55}, {GR_HALL, GS_OCTAVE, 1}, {2, 2, 0, 1}, 14, {3, 3, 1, 2}, 14, 0, {0, 0, 0, 0}},
	/* Boards of Canada: wide Juno chords (chorus II) on the bar, a Glow, soft swung rings, a
	 * bass; with vinyl and its warble */
	{"Hazy Kodak", {GV_STRINGS, GV_GLOW, GV_PULSE, GV_BASS},
	 {{30, 30, 70, 50, 25, 55, 50, 100, 68}, {40, 40, 60, 50, 40, 55, 60, 70, 55},
	  {45, 30, 35, 50, 10, 20, 50, 35, 53}, {25, 20, 80, 50, 0, 15, 50, 40, 65}},
	 {62, 35, 55, 40, 4, 1, 35, 50}, {GR_CHAMBER, GS_OCTAVE, 2}, {2, 2, 1, 0}, 12, {3, 3, 2, 1}, 12, 0, {0, 0, 0, 0}},
	/* Jean-Michel Jarre (Oxygene): the Seq bouncing, Juno chords on the half bar, a supersaw
	 * swelling, soft euclidean rings; key travel in lifts */
	{"Oxygen", {GV_SEQ, GV_STRINGS, GV_SUPER, GV_PULSE},
	 {{85, 45, 35, 50, 35, 45, 40, 0, 66}, {45, 45, 60, 50, 25, 60, 60, 60, 54},
	  {30, 40, 75, 50, 20, 70, 50, 40, 48}, {40, 35, 30, 50, 15, 25, 50, 25, 54}},
	 {92, 40, 65, 50, 0, 5, 40, 60}, {GR_HALL, GS_OCTAVE, 0}, {2, 1, 1, 0}, 10, {3, 2, 2, 1}, 10,
	 GL_FLAG_PINGPONG | (2u << 2), {0, 0, 0, 0}},
	/* ambient techno (after Gas): a soft four-on-the-floor kick, dark Juno chords off the beat,
	 * a pad and a drone, all in a foggy Bloom */
	{"Fog Machine", {GV_PULSE, GV_STRINGS, GV_PAD, GV_DRONE},
	 {{80, 20, 30, 50, 10, 45, 50, 53, 55}, {60, 25, 25, 50, 40, 60, 55, 40, 61},
	  {25, 25, 90, 50, 10, 85, 50, 60, 48}, {35, 30, 70, 25, 10, 70, 50, 0, 44}},
	 {98, 40, 75, 42, 7, 5, 20, 55}, {GR_BLOOM, GS_OCTAVE, 0}, {0, 4, 1, 0}, 8, {1, 4, 2, 1}, 8, 0, {0, 0, 0, 0}},
	/* minimal (after Glass and Reich): two Glows a third apart in eighths, the Seq in broken
	 * thirds, Juno chords on the bar; one key, slowly turning */
	{"Glass Tower", {GV_GLOW, GV_GLOW, GV_SEQ, GV_STRINGS},
	 {{95, 45, 40, 75, 15, 40, 35, 60, 68}, {95, 45, 40, 75, 15, 40, 65, 60, 60},
	  {90, 45, 30, 50, 15, 35, 50, 33, 71}, {25, 40, 70, 50, 15, 55, 50, 40, 63}},
	 {92, 20, 45, 50, 2, 0, 20, 35}, {GR_CHAMBER, GS_OCTAVE, 5}, {1, 1, 1, 0}, 8, {2, 2, 2, 1}, 8, 0, {0, 1u << 2, 0, 0}},
	/* a slow space: a supersaw two octaves down to one, a Glow far above, Juno chords ringing
	 * long, gongs; reverse swells, macros on a long rise */
	{"Low Orbit", {GV_SUPER, GV_GLOW, GV_STRINGS, GV_GONG},
	 {{30, 40, 80, 25, 15, 75, 50, 50, 74}, {30, 45, 75, 100, 45, 70, 60, 60, 60},
	  {15, 35, 90, 25, 25, 70, 45, 30, 63}, {30, 40, 80, 25, 20, 75, 50, 60, 65}},
	 {20, 45, 85, 50, 1, 2, 15, 75}, {GR_BLOOM, GS_OCTAVE, 4}, {3, 2, 1, 1}, 20, {4, 3, 2, 2}, 20,
	 GL_FLAG_REVERSE | (2u << 5), {0, 0, 0, 0}},
	/* ---- the four newest voices, and the styles asked for ---- */
	/* Vangelis's CS-80 (Blade Runner): a Ribbon line swooping in, the hand pressing it brighter
	 * and into vibrato; low chords, a pad, glass bells; a long hall */
	{"Off-World", {GV_RIBBON, GV_STRINGS, GV_PAD, GV_BELLS},
	 {{45, 55, 70, 50, 35, 75, 50, 15, 72}, {25, 35, 70, 25, 15, 70, 50, 60, 61},
	  {40, 30, 80, 50, 10, 70, 50, 40, 55}, {40, 45, 75, 75, 40, 80, 60, 30, 55}},
	 {30, 45, 88, 50, 2, 5, 20, 45}, {GR_HALL, GS_OCTAVE, 1}, {2, 0, 1, 1}, 12, {4, 2, 3, 2}, 12, 0, {0, 0, 0, 0}},
	/* the Ribbon as a reed, a harmonica's nasal pulse, over rolled harp chords and a drone */
	{"Harmonica Rain", {GV_RIBBON, GV_GLOW, GV_HARP, GV_DRONE},
	 {{50, 45, 55, 50, 40, 60, 45, 90, 100}, {60, 40, 60, 75, 45, 70, 65, 60, 95},
	  {60, 45, 60, 50, 30, 70, 35, 50, 100}, {30, 30, 50, 50, 0, 60, 50, 0, 55}},
	 {40, 40, 75, 50, 7, 4, 20, 40}, {GR_CHAMBER, GS_OCTAVE, 0}, {1, 1, 1, 0}, 10, {3, 3, 4, 1}, 10, 0, {0, 0, 0, 0}},
	/* French house: a slapped Funk line, off-beat chord stabs, four on the floor, a Seq high up,
	 * ping-pong eighths (with the pump and the auto filter in the demo) */
	{"Disco Filter", {GV_FUNK, GV_STRINGS, GV_PULSE, GV_SEQ},
	 {{70, 40, 35, 50, 0, 10, 50, 55, 70}, {70, 55, 30, 50, 30, 40, 50, 70, 51},
	  {80, 55, 30, 50, 0, 15, 50, 60, 56}, {85, 40, 25, 75, 35, 30, 55, 0, 37}},
	 {100, 30, 40, 50, 9, 1, 10, 35}, {GR_CHAMBER, GS_OCTAVE, 5}, {1, 2, 0, 1}, 8, {3, 4, 2, 3}, 8,
	 GL_FLAG_PINGPONG, {0, 0, 0, 0}},
	/* lo-fi house: dusty wide chords, a fingered Funk bass, a swung beat, an EP, the top rolled
	 * off (with vinyl in the demo) */
	{"Lofi House", {GV_STRINGS, GV_FUNK, GV_PULSE, GV_EP},
	 {{50, 30, 45, 50, 35, 50, 50, 90, 55}, {45, 25, 50, 50, 0, 10, 50, 10, 69},
	  {78, 30, 35, 50, 0, 20, 50, 75, 55}, {35, 30, 60, 50, 40, 55, 60, 40, 40}},
	 {98, 35, 50, 40, 5, 1, 15, 30}, {GR_CHAMBER, GS_OCTAVE, 6}, {2, 1, 0, 1}, 10, {4, 2, 2, 3}, 10, 0, {0, 0, 0, 0}},
	/* trap at half time: a low kick, sixteenth hats, the snare on the three; a long gliding 808,
	 * dark glass bells, a pad */
	{"Glass Trap", {GV_PULSE, GV_BASS, GV_BELLS, GV_PAD},
	 {{70, 60, 40, 25, 0, 15, 50, 55, 77}, {60, 10, 70, 50, 0, 5, 50, 60, 64},
	  {65, 35, 50, 50, 40, 55, 55, 45, 60}, {30, 25, 70, 50, 10, 60, 50, 30, 44}},
	 {50, 35, 55, 50, 1, 2, 15, 30}, {GR_HALL, GS_OCTAVE, 7}, {0, 1, 1, 0}, 8, {2, 3, 3, 2}, 8, 0, {0, 0, 0, 0}},
	/* Burzum's dungeon ambient (Tomhet): a plain flute line again and again, a choir-like pad,
	 * the harpsichord's lute stop, a drone; dark, slow, in a cold hall (tape hiss in the demo) */
	{"Tomhet", {GV_FLUTE, GV_PAD, GV_HARPSI, GV_DRONE},
	 {{40, 35, 70, 50, 30, 75, 45, 20, 70}, {50, 25, 85, 50, 10, 70, 50, 50, 53},
	  {25, 30, 40, 50, 30, 65, 60, 0, 79}, {20, 25, 60, 50, 0, 50, 50, 0, 34}},
	 {25, 40, 85, 42, 9, 5, 10, 15}, {GR_HALL, GS_OCTAVE, 3}, {0, 1, 0, 0}, 20, {2, 3, 2, 1}, 20, 0, {0, 0, 0, 0}},
	/* Autechre's All End: one smeared sound for a long time, feeding back on itself: a drone,
	 * shimmer and tails through the string echo into the Bloom, every macro slowly rising */
	{"All End", {GV_DRONE, GV_SHIMMER, GV_TAILS, GV_SWELL},
	 {{20, 50, 80, 50, 40, 80, 50, 25, 35}, {70, 40, 80, 50, 50, 90, 40, 50, 48},
	  {70, 35, 80, 50, 60, 85, 60, 60, 48}, {40, 35, 90, 25, 30, 80, 50, 50, 39}},
	 {10, 70, 92, 50, 4, 3, 30, 60}, {GR_BLOOM, GS_FIFTH, GL_ECHO_STRING}, {3, 3, 3, 3}, 30, {4, 4, 4, 4}, 40,
	 (2u << 5), {0, 0, 0, 0}},
	/* Nyman (The Draughtsman's Contract) outdone: the harpsichord in sixteenths on 8' and 4',
	 * strings stabbing in eighths, the Ribbon as the band's brass, a ground bass; the macros
	 * moving in blocks, bar by bar */
	{"Draughtsman", {GV_HARPSI, GV_STRINGS, GV_RIBBON, GV_BASS},
	 {{90, 70, 35, 50, 15, 35, 40, 100, 60}, {85, 65, 25, 50, 10, 35, 60, 40, 55},
	  {60, 65, 45, 50, 15, 40, 50, 35, 55}, {80, 45, 30, 50, 0, 10, 50, 50, 66}},
	 {92, 30, 40, 50, 2, 5, 30, 40}, {GR_CHAMBER, GS_OCTAVE, 7}, {1, 1, 2, 0}, 6, {3, 4, 3, 2}, 6,
	 (1u << 5), {0, 0, 0, 0}},
	/* more Nyman than Nyman: two harpsichords a third apart, one in sixteenths, one in
	 * eighths; strings driving in eighths, a running bass */
	{"Nyman Nyman", {GV_HARPSI, GV_HARPSI, GV_STRINGS, GV_BASS},
	 {{90, 70, 30, 50, 10, 35, 30, 50, 55}, {70, 60, 40, 50, 10, 35, 70, 100, 62},
	  {90, 60, 20, 50, 0, 30, 50, 30, 55}, {80, 50, 25, 50, 0, 10, 50, 70, 64}},
	 {100, 25, 35, 50, 9, 5, 40, 50}, {GR_CHAMBER, GS_OCTAVE, 5}, {1, 1, 2, 1}, 6, {4, 3, 4, 3}, 6,
	 (1u << 5), {0, 4, 0, 0}},
	/* harp glissandi washing up and down a Lydian scale, a Glow above, a pad and a soft bass */
	{"Harp Lagoon", {GV_HARP, GV_GLOW, GV_PAD, GV_BASS},
	 {{55, 50, 70, 50, 30, 70, 45, 100, 74}, {60, 45, 60, 75, 45, 70, 65, 60, 60},
	  {45, 30, 80, 50, 10, 70, 50, 40, 54}, {30, 20, 70, 50, 0, 20, 50, 30, 66}},
	 {40, 40, 80, 50, 0, 3, 25, 45}, {GR_SHIMMER, GS_OCTAVE, 0}, {1, 1, 0, 0}, 10, {4, 3, 2, 1}, 10, 0, {0, 0, 0, 0}},
	/* funk: a slapped line with the envelope filter's quack, a swung beat, chord stabs, the
	 * Ribbon as a lead synth up high */
	{"Funk Machine", {GV_FUNK, GV_PULSE, GV_STRINGS, GV_RIBBON},
	 {{80, 50, 30, 50, 0, 10, 50, 90, 75}, {70, 60, 35, 50, 0, 15, 50, 70, 60},
	  {75, 55, 25, 50, 20, 35, 55, 50, 50}, {55, 70, 40, 75, 25, 40, 45, 50, 66}},
	 {80, 30, 35, 50, 4, 1, 10, 35}, {GR_CHAMBER, GS_OCTAVE, 6}, {2, 0, 1, 1}, 8, {4, 3, 3, 3}, 8, 0, {0, 0, 0, 0}},
	/* ---- the Kit (after the 1997 drum recipes): CHARACTER is its split, and the macro moves it;
	 * dry, as those records' drums are (no echo, no reverb on its channel) ---- */
	/* lofi: the core only (kick, R-8 snare, metal hat, clank), clean, with an EP and a pad */
	{"Canvas Room", {GV_KIT, GV_EP, GV_BASS, GV_PAD},
	 {{55, 45, 35, 50, 0, 0, 50, 15, 89}, {40, 45, 55, 50, 25, 45, 40, 40, 63},
	  {35, 30, 80, 50, 5, 15, 50, 30, 63}, {40, 30, 70, 50, 10, 60, 60, 30, 45}},
	 {64, 30, 45, 50, 2, 1, 20, 35}, {GR_CHAMBER, GS_OCTAVE, 3}, {1, 1, 0, 0}, 8, {3, 2, 2, 1}, 8, 0, {0, 0, 0, 0}},
	/* the shifted hats and the clank in front, a sequence and strings behind */
	{"Shifted Metal", {GV_KIT, GV_SEQ, GV_BASS, GV_STRINGS},
	 {{65, 60, 45, 50, 0, 0, 50, 55, 83}, {55, 45, 40, 50, 30, 35, 40, 45, 51},
	  {45, 35, 60, 50, 0, 10, 50, 40, 64}, {30, 35, 75, 50, 15, 55, 60, 30, 37}},
	 {75, 35, 50, 50, 9, 5, 15, 40}, {GR_HALL, GS_OCTAVE, 6}, {2, 1, 0, 1}, 8, {4, 3, 2, 2}, 8, 0, {0, 0, 0, 0}},
	/* the whole kit: crunch, the Nord hits, rolls into each phrase, open hats; Phrygian */
	{"Crunch Roll", {GV_KIT, GV_GLOW, GV_DRONE, GV_SEQ},
	 {{75, 55, 55, 50, 0, 0, 50, 85, 90}, {35, 35, 65, 50, 30, 55, 35, 40, 49},
	  {30, 25, 90, 50, 0, 45, 50, 30, 36}, {50, 50, 35, 25, 35, 30, 65, 60, 46}},
	 {80, 40, 50, 50, 4, 2, 20, 45}, {GR_CHAMBER, GS_OCTAVE, 5}, {2, 1, 1, 1}, 8, {4, 2, 2, 3}, 8, 0, {0, 0, 0, 0}},
	/* ambient: few hits, long ones (tissue strokes, long hats), bells and a shimmer in a hall */
	{"Tissue Hall", {GV_KIT, GV_BELLS, GV_PAD, GV_SHIMMER},
	 {{25, 40, 50, 50, 0, 0, 50, 60, 85}, {35, 45, 60, 50, 40, 60, 40, 30, 64},
	  {50, 30, 80, 50, 15, 70, 60, 30, 64}, {25, 40, 70, 50, 20, 60, 55, 40, 42}},
	 {50, 45, 70, 50, 7, 3, 25, 40}, {GR_HALL, GS_OCTAVE, 7}, {3, 1, 1, 0}, 10, {4, 2, 2, 2}, 10, 0, {0, 0, 0, 0}},
	/* ---- the Kit's grooves (•• + FFWD on its channel), each at the tempo it has in mind ---- */
	/* UK garage two-step: 65 here, 130 felt; a swung kick that skips the 3, hats shuffling,
	 * the funk bass's sixteenths as garage eighths, EP chords */
	{"Two Step", {GV_KIT, GV_FUNK, GV_EP, GV_PAD},
	 {{55, 55, 35, 50, 0, 0, 50, 30, 95}, {55, 35, 40, 50, 0, 10, 50, 20, 65},
	  {45, 50, 45, 50, 25, 35, 40, 45, 53}, {35, 40, 60, 50, 10, 50, 60, 35, 36}},
	 {45, 30, 40, 50, 5, 1, 15, 35}, {GR_CHAMBER, GS_OCTAVE, 3}, {1, 1, 0, 0}, 8, {3, 2, 2, 1}, 8, 0, {4, 0, 0, 0}},
	/* drill: 70 here, 140 felt; sliding bass (the bass's glide all the way), hats in triplet
	 * bursts, a snare on the 3, dark strings and low bells */
	{"Drill Night", {GV_KIT, GV_BASS, GV_STRINGS, GV_BELLS},
	 {{60, 60, 40, 50, 0, 0, 50, 45, 87}, {40, 25, 80, 50, 0, 5, 50, 85, 65},
	  {30, 35, 70, 50, 10, 45, 55, 30, 38}, {30, 40, 55, 25, 25, 50, 40, 40, 33}},
	 {50, 30, 45, 50, 1, 5, 10, 30}, {GR_CHAMBER, GS_OCTAVE, 4}, {1, 1, 0, 0}, 8, {3, 2, 2, 2}, 8, 0, {8, 0, 0, 0}},
	/* jungle: 85 here, 170 felt; a chopped break, a sub an octave down, pads */
	{"Jungle Pressure", {GV_KIT, GV_BASS, GV_PAD, GV_GLOW},
	 {{70, 60, 40, 50, 0, 0, 50, 40, 77}, {35, 25, 85, 25, 0, 10, 50, 30, 57},
	  {40, 35, 75, 50, 15, 65, 50, 40, 33}, {35, 40, 60, 50, 30, 55, 35, 40, 28}},
	 {65, 40, 50, 50, 9, 5, 20, 45}, {GR_HALL, GS_OCTAVE, 5}, {2, 1, 1, 0}, 8, {4, 2, 2, 2}, 8, 0, {12, 0, 0, 0}},
	/* downtempo: 85 in sixteenths, swung; a lazy kick, the snare on 2 and 4, an EP and Glow */
	{"Slow Smoke", {GV_KIT, GV_EP, GV_BASS, GV_GLOW},
	 {{50, 40, 40, 50, 0, 0, 50, 25, 98}, {40, 40, 55, 50, 25, 40, 40, 40, 55},
	  {35, 30, 80, 50, 0, 10, 50, 35, 62}, {30, 35, 70, 50, 30, 55, 60, 40, 37}},
	 {65, 35, 45, 50, 2, 1, 15, 35}, {GR_BLOOM, GS_OCTAVE, 3}, {1, 1, 0, 1}, 8, {3, 2, 2, 2}, 8, 0, {16, 0, 0, 0}},
	/* house at 120: four on the floor, claps on 2 and 4, open hats on the off-beats, string stabs */
	{"Warehouse", {GV_KIT, GV_BASS, GV_STRINGS, GV_PAD},
	 {{60, 55, 35, 50, 0, 0, 50, 25, 80}, {50, 35, 45, 50, 0, 10, 50, 40, 55},
	  {55, 55, 30, 50, 20, 40, 45, 60, 40}, {35, 40, 70, 50, 10, 55, 60, 35, 30}},
	 {100, 30, 40, 50, 7, 5, 15, 35}, {GR_CHAMBER, GS_OCTAVE, 2}, {1, 1, 1, 0}, 8, {3, 2, 3, 2}, 8, 0, {20, 0, 0, 0}},
	/* thrash at 95 (190 felt): the skank beat, double kicks into the turnaround, the kit's
	 * crunch, saws for the guitars */
	{"Thrash Hall", {GV_KIT, GV_SUPER, GV_BASS, GV_DRONE},
	 {{75, 70, 30, 50, 0, 0, 50, 70, 71}, {55, 60, 40, 50, 0, 20, 45, 70, 40},
	  {60, 40, 40, 25, 0, 5, 50, 30, 49}, {30, 40, 90, 50, 0, 30, 55, 30, 27}},
	 {75, 25, 35, 50, 4, 2, 10, 50}, {GR_CHAMBER, GS_OCTAVE, 1}, {1, 1, 0, 0}, 8, {4, 3, 2, 2}, 8, 0, {24, 0, 0, 0}},
	/* reggaeton at 95: the dembow, a sub, plucks, a pad */
	{"Dembow", {GV_KIT, GV_BASS, GV_PIZZ, GV_PAD},
	 {{60, 55, 35, 50, 0, 0, 50, 20, 86}, {45, 30, 60, 25, 0, 5, 50, 30, 65},
	  {50, 55, 35, 50, 20, 35, 40, 45, 43}, {35, 40, 70, 50, 10, 55, 60, 35, 32}},
	 {75, 30, 40, 50, 2, 5, 15, 35}, {GR_CHAMBER, GS_OCTAVE, 3}, {1, 1, 0, 0}, 8, {3, 2, 2, 2}, 8, 0, {28, 0, 0, 0}},
	/* cumbia at 95: the kick on 1 and 3, a scraped tissue for the guiro, a walking bass,
	 * the muted brass as an accordion, plucks */
	{"Cumbia Lamp", {GV_KIT, GV_FUNK, GV_RIBBON, GV_PIZZ},
	 {{55, 55, 30, 50, 0, 0, 50, 35, 100}, {45, 35, 40, 50, 0, 10, 50, 15, 69},
	  {45, 55, 40, 50, 20, 35, 40, 40, 50}, {45, 50, 35, 50, 20, 30, 60, 40, 44}},
	 {75, 30, 40, 50, 7, 4, 15, 35}, {GR_CHAMBER, GS_OCTAVE, 3}, {1, 0, 1, 0}, 8, {3, 2, 3, 2}, 8, 0, {32, 0, 0, 0}},
	/* jazz at 70 (140 felt): a ride swung in triplets, ghosted comping on the snare, feathered
	 * kicks; an EP comping, a bass, a swell */
	{"Brush Club", {GV_KIT, GV_EP, GV_BASS, GV_SWELL},
	 {{50, 35, 40, 50, 0, 0, 50, 10, 100}, {45, 45, 50, 50, 20, 40, 40, 40, 68},
	  {50, 30, 55, 50, 0, 10, 50, 50, 75}, {25, 35, 70, 50, 15, 50, 60, 35, 34}},
	 {50, 30, 40, 50, 10, 1, 20, 40}, {GR_CHAMBER, GS_OCTAVE, 3}, {1, 1, 0, 0}, 8, {3, 2, 2, 2}, 8, 0, {36, 0, 0, 0}},
	/* IDM at 90: ratchets, skipped slots, the whole kit, and its macro at full (so now and then it
	 * rotates: the kick's hits on the snare, the snare's on the hat...) */
	{"Glitch Garden", {GV_KIT, GV_SEQ, GV_BELLS, GV_PAD},
	 {{65, 60, 35, 50, 0, 0, 50, 75, 84}, {50, 50, 40, 50, 30, 35, 40, 50, 42},
	  {35, 45, 55, 50, 40, 55, 60, 35, 37}, {35, 40, 70, 50, 10, 60, 50, 35, 32}},
	 {70, 35, 45, 50, 1, 3, 20, 45}, {GR_HALL, GS_OCTAVE, 4}, {4, 1, 1, 0}, 8, {4, 3, 2, 2}, 8, 0, {40, 0, 0, 0}},
	/* ---- lab pieces: a plain bed with one voice in front, to compare its methods ---- */
	{"Lab Strings", {GV_STRINGS, GV_BASS, GV_PULSE, GV_EP},
	 {{50, 45, 50, 50, 15, 60, 50, 50, 70}, {60, 30, 60, 50, 0, 15, 50, 30, 55},
	  {45, 40, 40, 50, 0, 20, 50, 20, 40}, {30, 30, 60, 50, 30, 50, 60, 30, 35}},
	 {64, 30, 60, 50, 2, 1, 20, 40}, {GR_HALL, GS_OCTAVE, 7}, {0, 0, 0, 0}, 0, {2, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	{"Lab Glow", {GV_GLOW, GV_GLOW, GV_PAD, GV_BASS},
	 {{55, 50, 55, 75, 35, 65, 40, 60, 100}, {35, 50, 55, 100, 40, 65, 60, 60, 80},
	  {40, 30, 80, 50, 10, 70, 50, 40, 24}, {40, 25, 70, 50, 0, 15, 50, 30, 32}},
	 {50, 40, 70, 50, 5, 0, 20, 40}, {GR_CHAMBER, GS_OCTAVE, 1}, {2, 2, 0, 0}, 8, {3, 3, 0, 0}, 8, 0, {0, 0, 0, 0}},
	{"Lab Ribbon", {GV_RIBBON, GV_PAD, GV_BASS, GV_PULSE},
	 {{55, 45, 55, 50, 25, 55, 50, 25, 95}, {40, 30, 80, 50, 10, 70, 50, 40, 32},
	  {50, 25, 60, 50, 0, 15, 50, 30, 42}, {40, 35, 35, 50, 0, 15, 50, 15, 30}},
	 {56, 30, 65, 50, 9, 5, 20, 35}, {GR_HALL, GS_OCTAVE, 7}, {1, 0, 0, 0}, 8, {3, 0, 0, 0}, 8, 0, {0, 0, 0, 0}},
	{"Lab Harp", {GV_HARP, GV_PAD, GV_BASS, GV_DRONE},
	 {{60, 45, 65, 50, 25, 70, 45, 60, 90}, {35, 30, 80, 50, 10, 70, 55, 40, 32},
	  {30, 20, 70, 50, 0, 20, 50, 30, 40}, {20, 25, 60, 50, 0, 50, 50, 0, 20}},
	 {44, 30, 75, 50, 7, 0, 25, 40}, {GR_HALL, GS_OCTAVE, 0}, {0, 0, 0, 0}, 0, {2, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
	{"Lab Kit", {GV_KIT, GV_PAD, GV_BASS, GV_EP},
	 {{60, 50, 50, 50, 0, 0, 50, 50, 95}, {40, 30, 70, 50, 10, 60, 50, 30, 37},
	  {40, 30, 70, 50, 0, 15, 50, 30, 57}, {30, 40, 60, 50, 20, 45, 60, 30, 37}},
	 {70, 30, 50, 50, 0, 5, 15, 35}, {GR_CHAMBER, GS_OCTAVE, 3}, {0, 0, 0, 0}, 0, {4, 0, 0, 0}, 0, 0, {0, 0, 0, 0}},
#endif
};

const char *gl_piece_name(int i) { return PIECES[(unsigned)i < GL_NPIECES ? i : 0].name; }

void gl_piece_macro(int i, uint8_t *steps, float *period_s)
{
	const recipe *r = &PIECES[(unsigned)i < GL_NPIECES ? i : 0];
	for (int c = 0; c < GL_NCH; c++)
		steps[c] = r->mac[c] > 4 ? 4 : r->mac[c];
	*period_s = r->mac_s ? (float)r->mac_s : 8.0f;
}

void gl_piece_scene(int i, uint8_t *steps, float *period_s)
{
	const recipe *r = &PIECES[(unsigned)i < GL_NPIECES ? i : 0];
	for (int c = 0; c < GL_NCH; c++)
		steps[c] = r->scene[c] > 4 ? 4 : r->scene[c];
	*period_s = r->scene_s ? (float)r->scene_s : 8.0f;
}

void gl_piece(int i, gl_patch *out)
{
	const recipe *r = &PIECES[(unsigned)i < GL_NPIECES ? i : 0];
	for (int c = 0; c < GL_NCH; c++) {
		out->voice[c] = r->voice[c];
		for (int p = 0; p < GL_NCP; p++)
			out->ch[c][p] = (float)r->ch[c][p] * 0.01f;
	}
	for (int p = 0; p < GL_NGP; p++)
		out->g[p] = (float)r->g[p] * 0.01f;
	for (int k = 0; k < 3; k++)
		out->space[k] = r->space[k];
	out->space[3] = r->flags;
	for (int c = 0; c < GL_NCH; c++)
		out->chx[c] = r->chx[c];
#ifdef GL_FORCE_REVERB                  /* for measuring each reverb's cost */
	out->space[GL_SP_REVERB] = GL_FORCE_REVERB;
#endif
	out->g[GP_ROOT] = (float)r->g[GP_ROOT] / 11.0f;
	out->g[GP_MODE] = (float)r->g[GP_MODE] / (float)(GL_NMODES - 1);
}

/* main thread: its own generator, so it never races the render's */
static uint32_t rr;
static float rrnd(void)
{
	rr ^= rr << 13;
	rr ^= rr >> 17;
	rr ^= rr << 5;
	return (float)(rr >> 8) * (1.0f / 16777216.0f);
}
static int rrndi(int n) { return (int)(rrnd() * (float)n) % n; }

void gl_randomize(gl_patch *p, uint32_t seed)
{
	rr = seed ? seed : 0x1234567u;
	for (int c = 0; c < GL_NCH; c++) {
		p->ch[c][CP_ACTIVITY] = 0.15f + 0.65f * rrnd();
		p->ch[c][CP_TONE] = 0.1f + 0.7f * rrnd();
		p->ch[c][CP_LENGTH] = 0.15f + 0.8f * rrnd();
		p->ch[c][CP_REGISTER] = (float)(1 + rrndi(3)) * 0.25f;   /* -1..+1 octave */
		p->ch[c][CP_ECHO] = 0.7f * rrnd();
		p->ch[c][CP_VERB] = 0.3f + 0.6f * rrnd();
		p->ch[c][CP_PAN] = 0.2f + 0.6f * rrnd();
		p->ch[c][CP_CHAR] = rrnd();
		p->ch[c][CP_LEVEL] = 0.4f + 0.4f * rrnd();
	}
	p->g[GP_TEMPO] = 0.19f + 0.54f * rrnd();                 /* 39..93 BPM */
	p->g[GP_DELAY] = 0.2f + 0.5f * rrnd();
	p->g[GP_REVERB] = 0.4f + 0.5f * rrnd();
	p->g[GP_FILTER] = 0.5f;                               /* never randomized into silence */
	p->g[GP_ROOT] = (float)rrndi(12) / 11.0f;
	p->g[GP_MODE] = (float)rrndi(GL_NMODES) / (float)(GL_NMODES - 1);
	p->g[GP_DRIFT] = 0.1f + 0.6f * rrnd();
	p->g[GP_MOTION] = 0.2f + 0.6f * rrnd();
	}
